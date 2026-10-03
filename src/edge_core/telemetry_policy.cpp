#include "telemetry_policy.hpp"
#include <cmath>
#include <stdexcept>

namespace mqmgateway::edge {
std::string TelemetryPolicy::key(const std::string& deviceId, const std::string& pointId) {
    return std::to_string(deviceId.size()) + ":" + deviceId + pointId;
}
bool TelemetryPolicy::configure(const std::string& deviceId, const std::string& pointId, ReportingPolicy policy) {
    if (deviceId.empty() || pointId.empty() || !std::isfinite(policy.absoluteDeadband) ||
        policy.absoluteDeadband < 0 || policy.maxReportInterval.count() < 0) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    const auto id = key(deviceId, pointId);
    if (!policies_.count(id) && policies_.size() >= 65536) return false;
    policies_[id] = policy;
    states_.erase(id);
    return true;
}
TelemetryPolicy::Decision TelemetryPolicy::process(
    const UnifiedMessageV2& message, PointValueType type, const std::function<bool()>& enqueue,
    std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    ++metrics_.valid;
    const auto id = key(message.deviceId, message.pointId);
    const auto policyIt = policies_.find(id);
    if (policyIt == policies_.end() || !policyIt->second.covEnabled) {
        if (!enqueue()) return Decision::queueRejected;
        ++metrics_.published;
        return Decision::published;
    }
    const auto stateIt = states_.find(id);
    const auto& policy = policyIt->second;
    bool changed = stateIt == states_.end() || stateIt->second.quality != message.quality;
    bool forced = false;
    if (!changed) {
        if (type == PointValueType::integer || type == PointValueType::floating) {
            auto number = [](const std::string& value) -> double {
                std::size_t used = 0;
                const double parsed = std::stod(value, &used);
                if (used != value.size() || !std::isfinite(parsed)) throw std::invalid_argument("invalid mapped numeric value");
                return parsed;
            };
            try {
                const double difference = std::abs(number(message.cookedValue) - number(stateIt->second.value));
                changed = policy.absoluteDeadband == 0 ? difference != 0 : difference >= policy.absoluteDeadband;
            } catch (const std::exception&) {
                // Never suppress an unparseable mapped value.
                changed = true;
            }
        } else {
            changed = stateIt->second.value != message.cookedValue;
        }
        if (!changed && policy.maxReportInterval.count() > 0 &&
            now - stateIt->second.publishedAt >= policy.maxReportInterval) {
            forced = true;
        }
    }
    if (!changed && !forced) {
        ++metrics_.suppressedCov;
        return Decision::suppressed;
    }
    // The callback only performs a bounded local queue insertion, never network I/O.
    if (!enqueue()) return Decision::queueRejected;
    states_[id] = {message.cookedValue, message.quality, now};
    ++metrics_.published;
    if (forced) ++metrics_.forcedMaxInterval;
    return Decision::published;
}
TelemetryPolicy::Metrics TelemetryPolicy::metrics() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return metrics_;
}
std::size_t TelemetryPolicy::stateSize() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return states_.size();
}
} // namespace mqmgateway::edge

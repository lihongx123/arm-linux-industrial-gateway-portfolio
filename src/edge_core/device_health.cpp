#include "device_health.hpp"
#include "diagnostic_json.hpp"
#include <algorithm>
#include <stdexcept>

namespace mqmgateway::edge {
const char* toString(HealthState state) {
    switch (state) {
    case HealthState::unknown: return "unknown";
    case HealthState::online: return "online";
    case HealthState::degraded: return "degraded";
    case HealthState::offline: return "offline";
    }
    return "unknown";
}
DeviceHealth::DeviceHealth(HealthConfig config, const TimeService& time)
    : config_(config), time_(time) {
    if (!config_.failureThreshold || !config_.recoveryThreshold || config_.staleAfter.count() < 0)
        throw std::invalid_argument("invalid device health policy");
}
bool DeviceHealth::registerDevice(const std::string& deviceId) {
    if (deviceId.empty() || devices_.size() >= 65536) return false;
    const auto now = time_.monotonic();
    return devices_.emplace(deviceId, Record{{}, now, now, false}).second;
}
std::optional<HealthTransition> DeviceHealth::change(const std::string& deviceId, Record& record,
                                                      HealthState next) {
    const auto previous = record.snapshot.state;
    if (previous == next) return std::nullopt;
    record.snapshot.state = next;
    return HealthTransition{deviceId, previous, next, time_.epochMs()};
}
std::optional<HealthTransition> DeviceHealth::observe(const UnifiedMessageV2& message) {
    const auto it = devices_.find(message.deviceId);
    if (it == devices_.end()) return std::nullopt;
    auto& record = it->second;
    auto& snapshot = record.snapshot;
    const bool goodTelemetry = message.dataType == iot::DataType::telemetry &&
                               message.quality == iot::Quality::good;
    const bool badTelemetry = message.dataType == iot::DataType::telemetry && !goodTelemetry;
    const bool badStatus = message.dataType == iot::DataType::status &&
        (message.status == "failed" || message.status == "error" || message.status == "timeout" ||
         message.status == "unavailable" || message.status == "unknown-result" ||
         message.status == "rejected");
    if (goodTelemetry) {
        ++snapshot.goodMessages;
        if (snapshot.consecutiveGood < config_.recoveryThreshold) ++snapshot.consecutiveGood;
        snapshot.consecutiveFailures = 0;
        record.lastGoodAt = time_.monotonic(); record.hasGood = true;
        snapshot.lastGoodEpochMs = time_.epochMs();
        if (snapshot.state == HealthState::unknown ||
            snapshot.consecutiveGood >= config_.recoveryThreshold)
            return change(message.deviceId, record, HealthState::online);
    } else if (badTelemetry || badStatus) {
        ++snapshot.failures;
        if (snapshot.consecutiveFailures < config_.failureThreshold) ++snapshot.consecutiveFailures;
        snapshot.consecutiveGood = 0;
        snapshot.lastErrorEpochMs = time_.epochMs();
        snapshot.lastError = message.detail.substr(0, 120);
        if (snapshot.consecutiveFailures >= config_.failureThreshold &&
            snapshot.state != HealthState::offline)
            return change(message.deviceId, record, HealthState::degraded);
    }
    return std::nullopt;
}
std::vector<HealthTransition> DeviceHealth::evaluate() {
    std::vector<HealthTransition> changes;
    if (config_.staleAfter.count() == 0) return changes;
    const auto now = time_.monotonic();
    for (auto& entry : devices_) {
        auto& record = entry.second;
        const auto since = record.hasGood ? record.lastGoodAt : record.registeredAt;
        if (now - since >= config_.staleAfter)
            if (auto transition = change(entry.first, record, HealthState::offline))
                changes.push_back(*transition);
    }
    return changes;
}
std::optional<HealthSnapshot> DeviceHealth::snapshot(const std::string& deviceId) const {
    const auto it = devices_.find(deviceId);
    return it == devices_.end() ? std::nullopt : std::optional<HealthSnapshot>(it->second.snapshot);
}
bool DeviceHealth::writeDevice(std::ostream& out, const std::string& deviceId) const {
    const auto it = devices_.find(deviceId);
    if (it == devices_.end()) return false;
    const auto& s = it->second.snapshot;
    out << "{\"device_id\":\"" << diagnosticJsonEscape(deviceId)
        << "\",\"state\":\"" << toString(s.state)
        << "\",\"good_messages\":" << s.goodMessages << ",\"failures\":" << s.failures
        << ",\"consecutive_failures\":" << s.consecutiveFailures
        << ",\"last_good_epoch_ms\":" << s.lastGoodEpochMs
        << ",\"last_error_epoch_ms\":" << s.lastErrorEpochMs
        << ",\"last_error\":\"" << diagnosticJsonEscape(s.lastError) << "\"}";
    return true;
}
void DeviceHealth::appendMetrics(std::ostream& out) const {
    std::size_t unknown = 0, online = 0, degraded = 0, offline = 0;
    for (const auto& entry : devices_) {
        switch (entry.second.snapshot.state) {
        case HealthState::unknown: ++unknown; break;
        case HealthState::online: ++online; break;
        case HealthState::degraded: ++degraded; break;
        case HealthState::offline: ++offline; break;
        }
    }
    out << "{\"devices\":" << devices_.size() << ",\"unknown\":" << unknown
        << ",\"online\":" << online << ",\"degraded\":" << degraded
        << ",\"offline\":" << offline << ",\"details\":[";
    std::size_t emitted = 0;
    for (const auto& entry : devices_) {
        if (emitted == 64) break; // Bounded metrics snapshot even with many devices.
        if (emitted++) out << ',';
        writeDevice(out, entry.first);
    }
    out << "],\"details_truncated\":" << (devices_.size() > emitted ? "true" : "false") << '}';
}
}  // namespace mqmgateway::edge

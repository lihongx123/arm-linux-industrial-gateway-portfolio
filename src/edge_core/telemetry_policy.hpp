#pragma once
#include "point_registry.hpp"
#include "unified_message_v2.hpp"
#include <chrono>
#include <cstdint>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace mqmgateway::edge {
struct ReportingPolicy {
    bool covEnabled{false};
    double absoluteDeadband{0};
    std::chrono::milliseconds maxReportInterval{0};
};

class TelemetryPolicy {
public:
    enum class Decision { published, suppressed, queueRejected };
    struct Metrics {
        std::uint64_t valid{0}, published{0}, suppressedCov{0}, forcedMaxInterval{0};
    };
    bool configure(const std::string& deviceId, const std::string& pointId, ReportingPolicy policy);
    Decision process(const UnifiedMessageV2& message, PointValueType type,
                     const std::function<bool()>& enqueue,
                     std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now());
    Metrics metrics() const;
    std::size_t stateSize() const;
private:
    struct State {
        std::string value;
        iot::Quality quality{iot::Quality::good};
        std::chrono::steady_clock::time_point publishedAt{};
    };
    static std::string key(const std::string& deviceId, const std::string& pointId);
    mutable std::mutex mutex_;
    std::unordered_map<std::string, ReportingPolicy> policies_;
    std::unordered_map<std::string, State> states_;
    Metrics metrics_;
};
} // namespace mqmgateway::edge

#pragma once

#include "time_service.hpp"
#include "unified_message_v2.hpp"
#include <chrono>
#include <cstdint>
#include <map>
#include <optional>
#include <ostream>
#include <string>
#include <vector>

namespace mqmgateway::edge {

enum class HealthState { unknown, online, degraded, offline };
const char* toString(HealthState state);

struct HealthConfig {
    unsigned failureThreshold{3};
    unsigned recoveryThreshold{2};
    // Zero disables silence-based offline detection; passive buses have no
    // inherent sampling interval, so a deployment must opt in deliberately.
    std::chrono::milliseconds staleAfter{0};
};

struct HealthTransition {
    std::string deviceId;
    HealthState from{HealthState::unknown};
    HealthState to{HealthState::unknown};
    std::int64_t epochMs{0};
};

struct HealthSnapshot {
    HealthState state{HealthState::unknown};
    std::uint64_t goodMessages{0}, failures{0};
    unsigned consecutiveGood{0}, consecutiveFailures{0};
    std::int64_t lastGoodEpochMs{0}, lastErrorEpochMs{0};
    std::string lastError;
};

// Owned by DiagnosticsManager; callers serialize access with its mutex.
class DeviceHealth {
public:
    DeviceHealth(HealthConfig config, const TimeService& time);
    bool registerDevice(const std::string& deviceId);
    std::optional<HealthTransition> observe(const UnifiedMessageV2& message);
    std::vector<HealthTransition> evaluate();
    std::optional<HealthSnapshot> snapshot(const std::string& deviceId) const;
    bool writeDevice(std::ostream& out, const std::string& deviceId) const;
    void appendMetrics(std::ostream& out) const;
private:
    struct Record {
        HealthSnapshot snapshot;
        TimeService::Monotonic::time_point registeredAt;
        TimeService::Monotonic::time_point lastGoodAt;
        bool hasGood{false};
    };
    std::optional<HealthTransition> change(const std::string& deviceId, Record& record, HealthState next);
    HealthConfig config_;
    const TimeService& time_;
    std::map<std::string, Record> devices_;
};

}  // namespace mqmgateway::edge

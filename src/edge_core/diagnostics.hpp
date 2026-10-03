#pragma once

#include "alarm_event.hpp"
#include "device_health.hpp"
#include "time_service.hpp"
#include <mutex>
#include <optional>

namespace mqmgateway::edge {

class DiagnosticsManager {
public:
    explicit DiagnosticsManager(HealthConfig config = {}, TimeService time = TimeService{});
    bool registerDevice(const std::string& deviceId);
    void observe(const UnifiedMessageV2& message);
    void evaluate();
    void reportQueueStall(bool stalled);
    bool acknowledgeAlarm(const std::string& key);
    std::vector<AlarmEvent> alarmEventsSince(std::uint64_t sequence) const;
    std::optional<HealthSnapshot> health(const std::string& deviceId) const;
    void write(std::ostream& out) const;
    void writeDevice(std::ostream& out, const std::string& deviceId) const;
    void appendMetrics(std::ostream& out) const;
private:
    mutable std::mutex mutex_;
    TimeService time_;
    DeviceHealth health_;
    AlarmLedger alarms_;
};

}  // namespace mqmgateway::edge

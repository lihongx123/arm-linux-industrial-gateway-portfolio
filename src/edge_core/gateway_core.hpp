#pragma once
#include "device_registry.hpp"
#include "diagnostics.hpp"
#include "driver_manager.hpp"
#include "point_registry.hpp"
#include <atomic>
#include <functional>
#include <memory>

namespace mqmgateway::edge {
// All driver emissions and commands pass through this protocol-neutral runtime.
class GatewayCore {
public:
    using Publish = std::function<void(UnifiedMessageV2)>;
    explicit GatewayCore(Publish publish, HealthConfig health = {}, TimeService time = TimeService{});
    ~GatewayCore();
    GatewayCore(const GatewayCore&) = delete;
    GatewayCore& operator=(const GatewayCore&) = delete;
    bool addDriver(std::shared_ptr<IDeviceDriver> driver);
    bool addDevice(DeviceDefinition device);
    bool addPoint(PointDefinition point);
    std::optional<PointDefinition> pointDefinition(const std::string& deviceId, const std::string& pointId) const;
    bool start();
    void stop() noexcept;
    bool prepare(UnifiedMessageV2& command);
    bool submit(const UnifiedMessageV2& command);
    std::string defaultCommand(const std::string& deviceId);
    std::vector<std::shared_ptr<IEventDrivenDriver>> eventDrivers() const;
    std::vector<std::shared_ptr<IAcquisitionDriver>> acquisitionDrivers() const;
    void evaluateDiagnostics();
    void reportQueueStall(bool stalled, const std::string& key = "queue_stalled");
    bool acknowledgeAlarm(const std::string& key);
    std::vector<AlarmEvent> alarmEventsSince(std::uint64_t sequence) const;
    std::optional<HealthSnapshot> deviceHealth(const std::string& deviceId) const;
    void writeDiagnostics(std::ostream& out) const;
    void writeDeviceDiagnostics(std::ostream& out, const std::string& deviceId) const;
    void appendMetrics(std::ostream& out) const;
    std::uint64_t mappingFailures() const { return mappingFailures_.load(); }
private:
    std::optional<DeviceDefinition> ensureDevice(const std::string& deviceId, const std::string& hint = {});
    void onDriverMessage(UnifiedMessageV2 message);
    Publish publish_;
    DeviceRegistry devices_;
    PointRegistry points_;
    DriverManager drivers_;
    DiagnosticsManager diagnostics_;
    std::atomic<std::uint64_t> received_{0}, dropped_{0}, submitted_{0}, rejected_{0}, mappingFailures_{0};
};
}  // namespace mqmgateway::edge

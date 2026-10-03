#pragma once
#include "device_driver.hpp"
#include "device_registry.hpp"
#include "point_registry.hpp"
#include <memory>
#include <optional>
#include <shared_mutex>
#include <string>
#include <unordered_map>
#include <vector>

namespace mqmgateway::edge {
class DriverManager {
public:
    bool add(std::shared_ptr<IDeviceDriver> driver);
    bool contains(const std::string& id) const;
    std::optional<DeviceDefinition> resolve(const std::string& deviceId, const std::string& hint = {}) const;
    std::string defaultCommand(const std::string& driverId) const;
    std::optional<PointDefinition> describePoint(const std::string& driverId, const UnifiedMessageV2& message) const;
    std::vector<std::shared_ptr<IEventDrivenDriver>> eventDrivers() const;
    std::vector<std::shared_ptr<IAcquisitionDriver>> acquisitionDrivers() const;
    bool startAll(IDeviceDriver::Emit emit = {});
    void stopAll() noexcept;
    bool submit(const DeviceRegistry& devices, const PointRegistry& points, const UnifiedMessageV2& command) const;
    void appendMetrics(std::ostream& out) const;
private:
    enum class State { stopped, starting, running, stopping };
    mutable std::shared_mutex mutex_;
    std::unordered_map<std::string, std::shared_ptr<IDeviceDriver>> drivers_;
    State state_{State::stopped};
};
}  // namespace mqmgateway::edge

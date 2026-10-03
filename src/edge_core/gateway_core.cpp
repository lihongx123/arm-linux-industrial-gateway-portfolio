#include "gateway_core.hpp"
#include "point_mapper.hpp"
#include <ostream>
#include <utility>

namespace mqmgateway::edge {
GatewayCore::GatewayCore(Publish publish, HealthConfig health, TimeService time)
    : publish_(std::move(publish)), diagnostics_(health, std::move(time)) {}
GatewayCore::~GatewayCore() { stop(); }
bool GatewayCore::addDriver(std::shared_ptr<IDeviceDriver> driver) { return drivers_.add(std::move(driver)); }
bool GatewayCore::addDevice(DeviceDefinition device) {
    if (!drivers_.contains(device.driverId)) return false;
    const auto id = device.id;
    if (!devices_.add(std::move(device))) return false;
    if (diagnostics_.registerDevice(id)) return true;
    devices_.remove(id);
    return false;
}
bool GatewayCore::addPoint(PointDefinition point) {
    return devices_.find(point.deviceId) && points_.add(std::move(point));
}
std::optional<DeviceDefinition> GatewayCore::ensureDevice(const std::string& deviceId, const std::string& hint) {
    auto device = devices_.find(deviceId);
    if (!device) {
        const auto owner = drivers_.resolve(deviceId, hint);
        if (!owner) return std::nullopt;
        if (devices_.add(*owner)) diagnostics_.registerDevice(deviceId);
        device = devices_.find(deviceId);
    }
    if (device && !hint.empty() && device->driverId != hint) return std::nullopt;
    return device;
}
bool GatewayCore::start() {
    return drivers_.startAll([this](UnifiedMessageV2 message) { onDriverMessage(std::move(message)); });
}
void GatewayCore::stop() noexcept { drivers_.stopAll(); }
bool GatewayCore::prepare(UnifiedMessageV2& command) {
    if (command.version != 2 || command.dataType != iot::DataType::command) return false;
    const auto device = ensureDevice(command.deviceId, command.driverId);
    if (!device) return false;
    const auto point = drivers_.describePoint(device->driverId, command);
    if (!point) return command.pointId.empty() || bool(points_.find(command.deviceId, command.pointId));
    if (point->deviceId != command.deviceId || point->id.empty() ||
        (!command.pointId.empty() && command.pointId != point->id)) return false;
    if (!points_.find(point->deviceId, point->id)) points_.add(*point);
    command.pointId = point->id;
    return bool(points_.find(command.deviceId, command.pointId));
}
bool GatewayCore::submit(const UnifiedMessageV2& command) {
    auto prepared = command;
    if (!prepare(prepared) || !drivers_.submit(devices_, points_, prepared)) {
        ++rejected_;
        return false;
    }
    ++submitted_;
    return true;
}
std::string GatewayCore::defaultCommand(const std::string& deviceId) {
    const auto device = ensureDevice(deviceId);
    return device ? drivers_.defaultCommand(device->driverId) : std::string{};
}
std::vector<std::shared_ptr<IEventDrivenDriver>> GatewayCore::eventDrivers() const { return drivers_.eventDrivers(); }
std::vector<std::shared_ptr<IAcquisitionDriver>> GatewayCore::acquisitionDrivers() const { return drivers_.acquisitionDrivers(); }
void GatewayCore::evaluateDiagnostics() { diagnostics_.evaluate(); }
void GatewayCore::reportQueueStall(bool stalled) { diagnostics_.reportQueueStall(stalled); }
bool GatewayCore::acknowledgeAlarm(const std::string& key) { return diagnostics_.acknowledgeAlarm(key); }
std::vector<AlarmEvent> GatewayCore::alarmEventsSince(std::uint64_t sequence) const {
    return diagnostics_.alarmEventsSince(sequence);
}
std::optional<HealthSnapshot> GatewayCore::deviceHealth(const std::string& deviceId) const {
    return diagnostics_.health(deviceId);
}
void GatewayCore::writeDiagnostics(std::ostream& out) const { diagnostics_.write(out); }
void GatewayCore::writeDeviceDiagnostics(std::ostream& out, const std::string& deviceId) const {
    diagnostics_.writeDevice(out, deviceId);
}
void GatewayCore::onDriverMessage(UnifiedMessageV2 message) {
    if (message.version != 2 || message.dataType == iot::DataType::command ||
        !ensureDevice(message.deviceId, message.driverId)) {
        ++dropped_;
        return;
    }
    // A synchronous command result already carries the point registered by prepare().
    // Avoid re-entering DriverManager's shared lock from a driver's submit callback.
    const auto known = !message.pointId.empty() ? points_.find(message.deviceId, message.pointId) : std::nullopt;
    const auto definition = known ? known : drivers_.describePoint(message.driverId, message);
    if (definition) {
        if (definition->deviceId != message.deviceId || definition->id.empty() ||
            (!message.pointId.empty() && message.pointId != definition->id)) { ++mappingFailures_; ++dropped_; return; }
        if (!points_.find(definition->deviceId, definition->id)) points_.add(*definition);
        const auto registered = points_.find(definition->deviceId, definition->id);
        if (!registered || !PointMapper::apply(message, *registered)) { ++mappingFailures_; ++dropped_; return; }
    } else if (!message.pointId.empty() && !points_.find(message.deviceId, message.pointId)) {
        ++mappingFailures_; ++dropped_; return;
    }
    ++received_;
    diagnostics_.observe(message);
    if (publish_) publish_(std::move(message));
}
void GatewayCore::appendMetrics(std::ostream& out) const {
    out << ",\"gateway_core\":{\"driver_messages\":" << received_.load()
        << ",\"dropped_messages\":" << dropped_.load()
        << ",\"commands_submitted\":" << submitted_.load()
        << ",\"commands_rejected\":" << rejected_.load()
        << ",\"mapping_failures\":" << mappingFailures_.load() << '}';
    drivers_.appendMetrics(out);
    diagnostics_.appendMetrics(out);
}
}  // namespace mqmgateway::edge

#include "driver_manager.hpp"
#include <mutex>

namespace mqmgateway::edge {
bool DriverManager::add(std::shared_ptr<IDeviceDriver> driver) {
    if (!driver || driver->id().empty()) return false;
    std::unique_lock<std::shared_mutex> lock(mutex_);
    if (state_ != State::stopped) return false;
    const auto id = driver->id();
    return drivers_.emplace(id, std::move(driver)).second;
}
bool DriverManager::contains(const std::string& id) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    return drivers_.find(id) != drivers_.end();
}
std::optional<DeviceDefinition> DriverManager::resolve(const std::string& deviceId, const std::string& hint) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::optional<DeviceDefinition> result;
    for (const auto& entry : drivers_) {
        if (!hint.empty() && entry.first != hint) continue;
        if (!entry.second->acceptsDevice(deviceId)) continue;
        if (result) return std::nullopt;
        result = DeviceDefinition{deviceId, entry.first};
    }
    return result;
}
std::string DriverManager::defaultCommand(const std::string& driverId) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = drivers_.find(driverId);
    return it == drivers_.end() ? std::string{} : it->second->defaultCommand();
}
std::optional<PointDefinition> DriverManager::describePoint(const std::string& driverId,
                                                             const UnifiedMessageV2& message) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    const auto it = drivers_.find(driverId);
    return it == drivers_.end() ? std::nullopt : it->second->describePoint(message);
}
std::vector<std::shared_ptr<IEventDrivenDriver>> DriverManager::eventDrivers() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<IEventDrivenDriver>> result;
    for (const auto& entry : drivers_) {
        auto driver = std::dynamic_pointer_cast<IEventDrivenDriver>(entry.second);
        if (driver) result.push_back(std::move(driver));
    }
    return result;
}
std::vector<std::shared_ptr<IAcquisitionDriver>> DriverManager::acquisitionDrivers() const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    std::vector<std::shared_ptr<IAcquisitionDriver>> result;
    for (const auto& entry : drivers_)
        if (auto driver = std::dynamic_pointer_cast<IAcquisitionDriver>(entry.second))
            result.push_back(std::move(driver));
    return result;
}
bool DriverManager::startAll(IDeviceDriver::Emit emit) {
    std::vector<std::shared_ptr<IDeviceDriver>> snapshot;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (state_ != State::stopped) return false;
        state_ = State::starting;
        for (const auto& entry : drivers_) snapshot.push_back(entry.second);
    }
    std::vector<std::shared_ptr<IDeviceDriver>> started;
    try {
        for (const auto& driver : snapshot) {
            const auto id = driver->id();
            driver->setMessageSink([emit, id](UnifiedMessageV2 message) {
                message.driverId = id;
                if (emit) emit(std::move(message));
            });
            started.push_back(driver);  // Unwind the driver whose start partially fails too.
            driver->start();
        }
    } catch (...) {
        for (auto it = started.rbegin(); it != started.rend(); ++it) {
            (*it)->stop();
            (*it)->setMessageSink({});
        }
        std::unique_lock<std::shared_mutex> lock(mutex_);
        state_ = State::stopped;
        throw;
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    state_ = State::running;
    return true;
}
void DriverManager::stopAll() noexcept {
    std::vector<std::shared_ptr<IDeviceDriver>> snapshot;
    {
        std::unique_lock<std::shared_mutex> lock(mutex_);
        if (state_ != State::running) return;
        state_ = State::stopping;
        for (const auto& entry : drivers_) snapshot.push_back(entry.second);
    }
    for (const auto& driver : snapshot) {
        driver->stop();
        driver->setMessageSink({});
    }
    std::unique_lock<std::shared_mutex> lock(mutex_);
    state_ = State::stopped;
}
bool DriverManager::submit(const DeviceRegistry& devices, const PointRegistry& points,
                           const UnifiedMessageV2& command) const {
    if (command.version != 2 || command.dataType != iot::DataType::command) return false;
    const auto device = devices.find(command.deviceId);
    if (!device || (!command.driverId.empty() && command.driverId != device->driverId)) return false;
    if (!command.pointId.empty()) {
        const auto point = points.find(command.deviceId, command.pointId);
        if (!point || !point->writable) return false;
    }
    std::shared_lock<std::shared_mutex> lock(mutex_);
    if (state_ != State::running) return false;
    const auto it = drivers_.find(device->driverId);
    return it != drivers_.end() && it->second->submit(command);
}
void DriverManager::appendMetrics(std::ostream& out) const {
    std::shared_lock<std::shared_mutex> lock(mutex_);
    for (const auto& entry : drivers_) entry.second->appendMetrics(out);
}
}  // namespace mqmgateway::edge

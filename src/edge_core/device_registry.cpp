#include "device_registry.hpp"

#include <utility>

namespace mqmgateway::edge {

bool DeviceRegistry::add(DeviceDefinition device) {
    if (device.id.empty() || device.driverId.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    // Dynamic field-bus discovery must not create an unbounded allocation path.
    if (devices_.size() >= 65536) return false;
    const auto id = device.id;
    return devices_.emplace(id, std::move(device)).second;
}

std::optional<DeviceDefinition> DeviceRegistry::find(const std::string& id) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = devices_.find(id);
    if (it == devices_.end()) return std::nullopt;
    return it->second;
}

bool DeviceRegistry::remove(const std::string& id) {
    std::lock_guard<std::mutex> lock(mutex_);
    return devices_.erase(id) != 0;
}

}  // namespace mqmgateway::edge

#include "point_registry.hpp"

#include <utility>

namespace mqmgateway::edge {

std::string PointRegistry::key(const std::string& deviceId, const std::string& pointId) {
    return std::to_string(deviceId.size()) + ":" + deviceId + pointId;
}

bool PointRegistry::add(PointDefinition point) {
    if (point.deviceId.empty() || point.id.empty()) return false;
    std::lock_guard<std::mutex> lock(mutex_);
    if (points_.size() >= 65536) return false;
    const auto pointKey = key(point.deviceId, point.id);
    return points_.emplace(pointKey, std::move(point)).second;
}

std::optional<PointDefinition> PointRegistry::find(const std::string& deviceId, const std::string& pointId) const {
    std::lock_guard<std::mutex> lock(mutex_);
    const auto it = points_.find(key(deviceId, pointId));
    if (it == points_.end()) return std::nullopt;
    return it->second;
}

bool PointRegistry::remove(const std::string& deviceId, const std::string& pointId) {
    std::lock_guard<std::mutex> lock(mutex_);
    return points_.erase(key(deviceId, pointId)) != 0;
}

}  // namespace mqmgateway::edge

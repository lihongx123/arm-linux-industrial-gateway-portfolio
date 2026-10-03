#pragma once

#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace mqmgateway::edge {

struct DeviceDefinition {
    std::string id;
    std::string driverId;
};

class DeviceRegistry {
public:
    bool add(DeviceDefinition device);
    std::optional<DeviceDefinition> find(const std::string& id) const;
    bool remove(const std::string& id);

private:
    mutable std::mutex mutex_;
    std::unordered_map<std::string, DeviceDefinition> devices_;
};

}  // namespace mqmgateway::edge

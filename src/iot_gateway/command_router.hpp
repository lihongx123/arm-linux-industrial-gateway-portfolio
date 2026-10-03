#pragma once

#include "unified_message.hpp"

#include <string>
#include <functional>

namespace mqmgateway::iot {

struct RouteResult {
    bool accepted{false};
    std::string deviceId;
    std::string command;
    std::string error;
    std::string driverId;
    UnifiedMessage message;
};

class CommandRouter {
public:
    RouteResult route(const std::string& topic, const std::string& payload) const;
    RouteResult routeCloud(const std::string& topic, const std::string& payload,
                           const std::function<std::string(const std::string&)>& resolve) const;
};

}  // namespace mqmgateway::iot

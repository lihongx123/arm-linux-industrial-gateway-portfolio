#pragma once

#include "unified_message.hpp"

#include <string>

namespace mqmgateway::iot {

struct RouteResult {
    bool accepted{false};
    std::string deviceId;
    std::string command;
    std::string error;
    UnifiedMessage message;
};

class CommandRouter {
public:
    RouteResult route(const std::string& topic, const std::string& payload) const;
};

}  // namespace mqmgateway::iot

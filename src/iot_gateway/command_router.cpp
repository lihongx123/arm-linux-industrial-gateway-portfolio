#include "command_router.hpp"

#include <rapidjson/document.h>

#include <charconv>
#include <cstdint>
#include <vector>

namespace mqmgateway::iot {
namespace {

bool decodeHex(const std::string& input, std::vector<std::uint8_t>& output) {
    if (input.size() > 16 || input.size() % 2 != 0) {
        return false;
    }
    output.clear();
    for (std::size_t index = 0; index < input.size(); index += 2) {
        unsigned int value = 0;
        const auto begin = input.data() + index;
        const auto end = begin + 2;
        const auto conversion = std::from_chars(begin, end, value, 16);
        if (conversion.ec != std::errc{} || conversion.ptr != end) {
            return false;
        }
        output.push_back(static_cast<std::uint8_t>(value));
    }
    return true;
}

}  // namespace

RouteResult CommandRouter::route(const std::string& topic, const std::string& payload) const {
    RouteResult result;
    const std::string prefix = "device/";
    const auto commandMarker = topic.find("/cmd/");
    if (topic.rfind(prefix, 0) != 0 || commandMarker == std::string::npos || commandMarker <= prefix.size()) {
        result.error = "topic must match device/{id}/cmd/{name}";
        return result;
    }
    result.deviceId = topic.substr(prefix.size(), commandMarker - prefix.size());
    result.command = topic.substr(commandMarker + 5);
    if (result.command != "can_tx" && result.command != "modbus_write") {
        result.error = "unsupported command: " + result.command;
        return result;
    }

    rapidjson::Document document;
    if (document.Parse(payload.c_str()).HasParseError() || !document.IsObject()) {
        result.error = "payload must be a JSON object";
        return result;
    }
    if (result.command == "modbus_write") {
        if (!document.HasMember("slave") || !document["slave"].IsUint() ||
            document["slave"].GetUint() < 1 || document["slave"].GetUint() > 247 ||
            !document.HasMember("register") || !document["register"].IsUint() || document["register"].GetUint() > 65535 ||
            !document.HasMember("value") || !document["value"].IsUint() || document["value"].GetUint() > 65535) {
            result.error = "Modbus requires slave 1..247, register/value 0..65535";
            return result;
        }
        result.message.protocol = Protocol::modbus_rtu;
        result.message.direction = Direction::southbound;
        result.message.dataType = DataType::command;
        result.message.deviceId = result.deviceId;
        result.message.slave = static_cast<std::uint8_t>(document["slave"].GetUint());
        result.message.address = document["register"].GetUint();
        if (result.deviceId != "rtu-" + std::to_string(result.message.slave)) {
            result.error = "Modbus device ID must match rtu-{slave}";
            return result;
        }
        const auto value = document["value"].GetUint();
        result.message.payload = {static_cast<std::uint8_t>(value >> 8U), static_cast<std::uint8_t>(value)};
        if (document.HasMember("timeout_ms")) {
            if (!document["timeout_ms"].IsUint() || document["timeout_ms"].GetUint() == 0) {
                result.error = "timeout_ms must be positive"; return result;
            }
            result.message.timeout = std::chrono::milliseconds(document["timeout_ms"].GetUint());
        }
        result.accepted = true;
        return result;
    }
    if (!document.HasMember("can_id") || !document["can_id"].IsUint()) {
        result.error = "can_id must be an unsigned integer";
        return result;
    }
    if (!document.HasMember("data") || !document["data"].IsString()) {
        result.error = "data must be an even-length hexadecimal string up to 16 characters";
        return result;
    }

    result.message.timestamp = std::chrono::system_clock::now();
    result.message.enqueuedAt = std::chrono::steady_clock::now();
    result.message.deviceId = result.deviceId;
    result.message.protocol = Protocol::can;
    result.message.direction = Direction::southbound;
    result.message.dataType = DataType::command;
    result.message.address = document["can_id"].GetUint();
    if (result.message.address > 0x1FFFFFFF) {
        result.error = "can_id exceeds the 29-bit CAN identifier range";
        return result;
    }
    if (!decodeHex(document["data"].GetString(), result.message.payload)) {
        result.error = "data must be an even-length hexadecimal string up to 16 characters";
        return result;
    }
    if (document.HasMember("timeout_ms")) {
        if (!document["timeout_ms"].IsUint() || document["timeout_ms"].GetUint() == 0) {
            result.error = "timeout_ms must be a positive unsigned integer";
            return result;
        }
        result.message.timeout = std::chrono::milliseconds(document["timeout_ms"].GetUint());
    }
    result.accepted = true;
    return result;
}

}  // namespace mqmgateway::iot

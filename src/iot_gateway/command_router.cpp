#include "command_router.hpp"

#include <rapidjson/document.h>

#include <charconv>
#include <cstdint>
#include <vector>

namespace mqmgateway::iot {
namespace {

bool decodeHex(const std::string& input, std::vector<std::uint8_t>& output, std::size_t maximum = 16) {
    if (input.size() > maximum || input.size() % 2 != 0) {
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
    if (result.command != "can_tx" && result.command != "modbus_write" &&
        result.command != "modbus_tcp_write" && result.command != "tcp_send" &&
        result.command != "mc_write" && result.command != "opcua_write" &&
        result.command != "s7_write" && result.command != "write") {
        result.error = "unsupported command: " + result.command;
        return result;
    }

    rapidjson::Document document;
    if (document.Parse(payload.c_str()).HasParseError() || !document.IsObject()) {
        result.error = "payload must be a JSON object";
        return result;
    }
    if (result.command == "tcp_send" || result.command == "write") {
        if (!document.HasMember("data") || !document["data"].IsString() ||
            !decodeHex(std::string(document["data"].GetString(), document["data"].GetStringLength()), result.message.payload, 8192) ||
            result.message.payload.empty()) {
            result.error = "TCP data requires 1..4096 hexadecimal bytes"; return result;
        }
        result.driverId = result.command == "tcp_send" ? "generic_tcp:" + result.deviceId : "";
        result.message.deviceId = result.deviceId;
        result.message.protocol = result.command == "tcp_send" ? Protocol::generic_tcp : Protocol::mqtt;
        result.message.direction = Direction::southbound;
        result.message.dataType = DataType::command;
        result.accepted = true;
        return result;
    }
    if (result.command == "mc_write") {
        if (!document.HasMember("register") || !document["register"].IsUint() ||
            document["register"].GetUint() > 0xFFFFFF ||
            !document.HasMember("value") || !document["value"].IsUint() ||
            document["value"].GetUint() > 65535) {
            result.error = "MC requires D-register 0..16777215 and value 0..65535";
            return result;
        }
        result.message.deviceId = result.deviceId;
        result.message.protocol = Protocol::mitsubishi_mc;
        result.message.direction = Direction::southbound;
        result.message.dataType = DataType::command;
        result.message.address = document["register"].GetUint();
        const auto value = document["value"].GetUint();
        result.message.payload = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        if (document.HasMember("timeout_ms")) {
            if (!document["timeout_ms"].IsUint() || document["timeout_ms"].GetUint() == 0) {
                result.error = "timeout_ms must be positive"; return result;
            }
            result.message.timeout = std::chrono::milliseconds(document["timeout_ms"].GetUint());
        }
        result.driverId = "mc:" + result.deviceId;
        result.accepted = true;
        return result;
    }
    if (result.command == "opcua_write") {
        if (!document.HasMember("value") || !document["value"].IsInt64() ||
            document["value"].GetInt64() < 0 || document["value"].GetInt64() > 0x7FFFFFFF) {
            result.error = "OPC UA integer write requires value 0..2147483647";
            return result;
        }
        result.message.deviceId = result.deviceId;
        result.message.protocol = Protocol::opcua;
        result.message.direction = Direction::southbound;
        result.message.dataType = DataType::command;
        const auto value = static_cast<std::uint32_t>(document["value"].GetInt64());
        result.message.payload = {static_cast<std::uint8_t>(value >> 24), static_cast<std::uint8_t>(value >> 16),
                                    static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        result.driverId = "opcua:" + result.deviceId;
        result.accepted = true;
        return result;
    }
    if (result.command == "s7_write") {
        if (!document.HasMember("value") || !document["value"].IsUint() || document["value"].GetUint() > 65535) {
            result.error = "S7 DB word write requires value 0..65535"; return result;
        }
        result.message.deviceId = result.deviceId; result.message.protocol = Protocol::siemens_s7;
        result.message.direction = Direction::southbound; result.message.dataType = DataType::command;
        const auto value = document["value"].GetUint();
        result.message.payload = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        result.driverId = "s7:" + result.deviceId; result.accepted = true; return result;
    }
    if (result.command == "modbus_write" || result.command == "modbus_tcp_write") {
        if (!document.HasMember("slave") || !document["slave"].IsUint() ||
            document["slave"].GetUint() < 1 || document["slave"].GetUint() > 247 ||
            !document.HasMember("register") || !document["register"].IsUint() || document["register"].GetUint() > 65535 ||
            !document.HasMember("value") || !document["value"].IsUint() || document["value"].GetUint() > 65535) {
            result.error = "Modbus requires slave 1..247, register/value 0..65535";
            return result;
        }
        const bool tcp = result.command == "modbus_tcp_write";
        result.message.protocol = tcp ? Protocol::modbus_tcp : Protocol::modbus_rtu;
        result.driverId = tcp ? "modbus_tcp:" + result.deviceId : "modbus_rtu";
        result.message.direction = Direction::southbound;
        result.message.dataType = DataType::command;
        result.message.deviceId = result.deviceId;
        result.message.slave = static_cast<std::uint8_t>(document["slave"].GetUint());
        result.message.address = document["register"].GetUint();
        if (!tcp && result.deviceId != "rtu-" + std::to_string(result.message.slave)) {
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
    result.driverId = "can";
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

RouteResult CommandRouter::routeCloud(const std::string& topic, const std::string& payload,
                                     const std::function<std::string(const std::string&)>& resolve) const {
    const std::string prefix = "resume/gateway/devices/";
    const std::string suffix = "/command";
    RouteResult result;
    if (topic.rfind(prefix, 0) != 0 || topic.size() <= prefix.size() + suffix.size() ||
        topic.compare(topic.size() - suffix.size(), suffix.size(), suffix) != 0 ||
        topic.find('/', prefix.size()) != topic.size() - suffix.size()) {
        result.error = "topic must match resume/gateway/devices/{id}/command";
        return result;
    }
    result.deviceId = topic.substr(prefix.size(), topic.size() - prefix.size() - suffix.size());
    const auto command = resolve(result.deviceId);
    if (command.empty()) {
        result.error = "device has no registered command driver";
        return result;
    }
    return route("device/" + result.deviceId + "/cmd/" + command, payload);
}

}  // namespace mqmgateway::iot

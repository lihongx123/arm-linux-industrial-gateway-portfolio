#include "unified_message.hpp"

#include <chrono>
#include <iomanip>
#include <sstream>

namespace mqmgateway::iot {
namespace {

std::string escapeJson(const std::string& value) {
    std::ostringstream output;
    for (const auto character : value) {
        if (character == '"' || character == '\\') {
            output << '\\';
        }
        output << character;
    }
    return output.str();
}

}  // namespace

std::string toString(const Protocol value) {
    switch (value) {
        case Protocol::modbus_rtu: return "modbus_rtu";
        case Protocol::modbus_tcp: return "modbus_tcp";
        case Protocol::can: return "can";
        case Protocol::mqtt: return "mqtt";
    }
    return "unknown";
}

std::string toString(const Direction value) {
    return value == Direction::northbound ? "northbound" : "southbound";
}

std::string toString(const DataType value) {
    switch (value) {
        case DataType::telemetry: return "telemetry";
        case DataType::command: return "command";
        case DataType::status: return "status";
        case DataType::heartbeat: return "heartbeat";
    }
    return "unknown";
}

std::string toString(const Quality value) {
    switch (value) {
        case Quality::good: return "good";
        case Quality::timeout: return "timeout";
        case Quality::crc_error: return "crc_error";
        case Quality::invalid: return "invalid";
        case Quality::unavailable: return "unavailable";
    }
    return "unknown";
}

std::string payloadToHex(const std::vector<std::uint8_t>& payload) {
    std::ostringstream output;
    output << std::hex << std::setfill('0');
    for (const auto byte : payload) {
        output << std::setw(2) << static_cast<unsigned int>(byte);
    }
    return output.str();
}

std::string toJson(const UnifiedMessage& message) {
    const auto timestamp = std::chrono::duration_cast<std::chrono::milliseconds>(
        message.timestamp.time_since_epoch()).count();
    std::ostringstream output;
    output << "{\"timestamp_ms\":" << timestamp
           << ",\"device_id\":\"" << escapeJson(message.deviceId)
           << "\",\"protocol\":\"" << toString(message.protocol)
           << "\",\"direction\":\"" << toString(message.direction)
           << "\",\"data_type\":\"" << toString(message.dataType)
           << "\",\"address\":" << message.address
           << ",\"payload\":\"" << payloadToHex(message.payload)
           << "\",\"quality\":\"" << toString(message.quality) << "\"}";
    return output.str();
}

}  // namespace mqmgateway::iot

#pragma once

#include <chrono>
#include <cstdint>
#include <string>
#include <vector>

namespace mqmgateway::iot {

enum class Protocol { modbus_rtu, modbus_tcp, can, mqtt, generic_tcp, spi, i2c, gpio, uart, mitsubishi_mc, opcua, siemens_s7 };
enum class Direction { southbound, northbound };
enum class DataType { telemetry, command, status, heartbeat };
enum class Quality { good, timeout, crc_error, invalid, unavailable };

struct UnifiedMessage {
    std::chrono::system_clock::time_point timestamp{std::chrono::system_clock::now()};
    std::string deviceId;
    std::string correlationId;
    Protocol protocol{Protocol::can};
    Direction direction{Direction::northbound};
    DataType dataType{DataType::telemetry};
    std::vector<std::uint8_t> payload;
    Quality quality{Quality::good};
    std::uint32_t address{0};
    std::uint8_t slave{1};
    std::chrono::steady_clock::time_point enqueuedAt{std::chrono::steady_clock::now()};
    std::chrono::milliseconds timeout{1000};
};

std::string toString(Protocol value);
std::string toString(Direction value);
std::string toString(DataType value);
std::string toString(Quality value);
std::string payloadToHex(const std::vector<std::uint8_t>& payload);
std::string toJson(const UnifiedMessage& message);

}  // namespace mqmgateway::iot

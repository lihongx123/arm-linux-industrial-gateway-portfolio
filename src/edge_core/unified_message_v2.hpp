#pragma once

#include "unified_message.hpp"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace mqmgateway::edge {

enum class NorthboundType { telemetry, attribute, status, alarm, diagnostic, command, command_result };

// Internal migration envelope. Legacy wire JSON is still generated from UnifiedMessage.
struct UnifiedMessageV2 {
    std::uint32_t version{2};
    std::string deviceId;
    std::string pointId;
    std::string correlationId;
    std::string sourceAdapterId;
    std::string operation;
    std::string commandState;
    std::uint64_t eventSequence{0};
    std::int64_t eventEpochMs{0};
    NorthboundType northboundType{NorthboundType::telemetry};
    std::optional<std::chrono::steady_clock::time_point> deadline;
    std::string driverId;
    std::string status;
    std::string detail;
    iot::Direction direction{iot::Direction::northbound};
    iot::DataType dataType{iot::DataType::telemetry};
    iot::Quality quality{iot::Quality::good};
    std::vector<std::uint8_t> rawPayload;
    // Point bytes may differ from the legacy wire frame (for example RTU CRC/header).
    std::vector<std::uint8_t> pointRawPayload;
    std::string rawValue;
    std::string cookedValue;
    std::string sourceDescriptor;
    std::optional<std::uint32_t> protocolStatusCode;
    std::optional<std::chrono::system_clock::time_point> sourceTimestamp;
    std::optional<std::chrono::system_clock::time_point> serverTimestamp;
    std::chrono::system_clock::time_point sourceTime{};
    std::chrono::steady_clock::time_point enqueuedAt{};
    std::chrono::milliseconds timeout{1000};
    // Kept only for a lossless V1 round trip during migration.
    iot::Protocol legacyProtocol{iot::Protocol::can};
    std::uint32_t legacyAddress{0};
    std::uint8_t legacySlave{1};
};

UnifiedMessageV2 fromLegacy(const iot::UnifiedMessage& message);
iot::UnifiedMessage toLegacy(const UnifiedMessageV2& message);

}  // namespace mqmgateway::edge

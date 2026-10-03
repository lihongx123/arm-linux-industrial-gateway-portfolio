#include "unified_message_v2.hpp"

#include <stdexcept>

namespace mqmgateway::edge {

UnifiedMessageV2 fromLegacy(const iot::UnifiedMessage& message) {
    UnifiedMessageV2 result;
    result.deviceId = message.deviceId;
    result.correlationId = message.correlationId;
    result.direction = message.direction;
    result.dataType = message.dataType;
    if (message.dataType == iot::DataType::status) result.northboundType = NorthboundType::status;
    if (message.dataType == iot::DataType::command) result.northboundType = NorthboundType::command;
    result.quality = message.quality;
    result.rawPayload = message.payload;
    result.sourceTime = message.timestamp;
    result.enqueuedAt = message.enqueuedAt;
    result.timeout = message.timeout;
    result.legacyProtocol = message.protocol;
    result.legacyAddress = message.address;
    result.legacySlave = message.slave;
    return result;
}

iot::UnifiedMessage toLegacy(const UnifiedMessageV2& message) {
    if (message.version != 2) {
        throw std::invalid_argument("unsupported internal message version");
    }
    iot::UnifiedMessage result;
    result.deviceId = message.deviceId;
    result.correlationId = message.correlationId;
    result.direction = message.direction;
    result.dataType = message.dataType;
    result.quality = message.quality;
    result.payload = message.rawPayload;
    result.timestamp = message.sourceTime;
    result.enqueuedAt = message.enqueuedAt;
    result.timeout = message.timeout;
    result.protocol = message.legacyProtocol;
    result.address = message.legacyAddress;
    result.slave = message.legacySlave;
    return result;
}

}  // namespace mqmgateway::edge

#include "mc_driver.hpp"
#include <stdexcept>

namespace mqmgateway::drivers {
namespace {
constexpr std::uint8_t dWordCode = 0xA8;
constexpr std::uint8_t requestHeader[] = {0x50, 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00};
constexpr std::uint8_t responseHeader[] = {0xD0, 0x00, 0x00, 0xFF, 0xFF, 0x03, 0x00};
void append16(std::vector<std::uint8_t>& bytes, unsigned value) {
    bytes.push_back(static_cast<std::uint8_t>(value));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
}
unsigned read16(const std::vector<std::uint8_t>& bytes, std::size_t offset) {
    return unsigned(bytes[offset]) | (unsigned(bytes[offset + 1]) << 8);
}
}  // namespace

McDriver::McDriver(McConfig config, std::size_t capacity)
    : TcpDriver(config, capacity), mc_(std::move(config)) {
    if (mc_.pointId.empty() ||
        mc_.pointId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
        mc_.registerAddress > 0xFFFFFF)
        throw std::invalid_argument("MC requires a stable point ID and 24-bit D address");
}

std::optional<edge::PointDefinition> McDriver::describePoint(const edge::UnifiedMessageV2& m) const {
    if (m.deviceId != mc_.deviceId || m.legacyAddress != mc_.registerAddress ||
        (!m.pointId.empty() && m.pointId != mc_.pointId)) return std::nullopt;
    return edge::PointDefinition{mc_.deviceId, mc_.pointId, edge::PointValueType::integer,
                                 "", true, mc_.registerAddress};
}

bool McDriver::valid(const edge::UnifiedMessageV2& m) const {
    return m.dataType == iot::DataType::command &&
           m.legacyAddress == mc_.registerAddress &&
           (m.pointId.empty() || m.pointId == mc_.pointId) && m.rawPayload.size() == 2;
}

void McDriver::preparePoll(edge::UnifiedMessageV2& message) const {
    message.pointId = mc_.pointId;
    message.legacyAddress = mc_.registerAddress;
}

TcpDriver::Bytes McDriver::encode(const edge::UnifiedMessageV2& message, bool command) {
    Bytes bytes(std::begin(requestHeader), std::end(requestHeader));
    append16(bytes, command ? 14 : 12);
    append16(bytes, 0x0010); // Monitoring timer: 16 x 250 ms, bounded by local responseMs.
    append16(bytes, command ? 0x1401 : 0x0401);
    append16(bytes, 0); // Word units.
    bytes.push_back(static_cast<std::uint8_t>(message.legacyAddress));
    bytes.push_back(static_cast<std::uint8_t>(message.legacyAddress >> 8));
    bytes.push_back(static_cast<std::uint8_t>(message.legacyAddress >> 16));
    bytes.push_back(dWordCode);
    append16(bytes, 1);
    if (command) {
        // The common PointMapper uses big-endian integer bytes; MC wire words are little-endian.
        bytes.push_back(message.rawPayload[1]);
        bytes.push_back(message.rawPayload[0]);
    }
    return bytes;
}

int McDriver::frameSize(const Bytes& input) const {
    for (std::size_t i = 0; i < input.size() && i < sizeof(responseHeader); ++i)
        if (input[i] != responseHeader[i]) return -1;
    if (input.size() < 9) return 0;
    const auto length = read16(input, 7);
    if (length < 2 || length > 32) return -1;
    return static_cast<int>(9 + length);
}

bool McDriver::consume(const Bytes& frame) {
    if (!mPending || mPending->offset != mPending->wire.size() || frame.size() < 11) return false;
    const auto endCode = read16(frame, 9);
    if (endCode != 0) {
        markFailure(3);
        finish("failed", "MC end code " + std::to_string(endCode));
        return true;
    }
    if (mPending->command) {
        if (frame.size() != 11) return false;
        markSuccess();
        finish("success", "MC write acknowledged by end code zero");
    } else {
        if (frame.size() != 13) return false;
        markSuccess();
        // Convert MC little-endian word to the common mapper's big-endian value bytes.
        emitTelemetry({frame[12], frame[11]}, mc_.registerAddress);
        finish("ok", "");
    }
    return true;
}
}  // namespace mqmgateway::drivers

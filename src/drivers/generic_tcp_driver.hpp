#pragma once
#include "tcp_driver.hpp"
namespace mqmgateway::drivers {
// Wire: unsigned 16-bit big-endian payload length, then 1..4096 opaque bytes.
class GenericTcpDriver final : public TcpDriver {
public:
    using TcpDriver::TcpDriver;
    std::string id() const override { return "generic_tcp:" + mConfig.deviceId; }
    std::string defaultCommand() const override { return "tcp_send"; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2& m) const override {
        return edge::PointDefinition{m.deviceId, "payload", edge::PointValueType::bytes, "", true, 0};
    }
protected:
    bool valid(const edge::UnifiedMessageV2& m) const override { return !m.rawPayload.empty() && m.rawPayload.size() <= 4096; }
    Bytes encode(const edge::UnifiedMessageV2& m, bool) override {
        Bytes b{static_cast<uint8_t>(m.rawPayload.size() >> 8), static_cast<uint8_t>(m.rawPayload.size())};
        b.insert(b.end(), m.rawPayload.begin(), m.rawPayload.end()); return b;
    }
    int frameSize(const Bytes& b) const override {
        if (b.size() < 2) return 0;
        const unsigned length = (b[0] << 8) | b[1];
        return !length || length > 4096 ? -1 : static_cast<int>(length + 2);
    }
    bool consume(const Bytes& b) override { emitTelemetry(Bytes(b.begin() + 2, b.end())); return true; }
    iot::Protocol protocol() const override { return iot::Protocol::generic_tcp; }
};
}

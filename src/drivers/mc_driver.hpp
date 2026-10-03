#pragma once
#include "tcp_driver.hpp"

namespace mqmgateway::drivers {
// MC 3E binary/TCP, CPU route, one D-area unsigned 16-bit word per point.
struct McConfig : TcpConfig {
    std::string pointId;
    unsigned registerAddress{0};
};

class McDriver final : public TcpDriver {
public:
    McDriver(McConfig config, std::size_t capacity);
    std::string id() const override { return "mc:" + mConfig.deviceId; }
    std::string defaultCommand() const override { return "mc_write"; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2& m) const override;
protected:
    bool valid(const edge::UnifiedMessageV2& m) const override;
    Bytes encode(const edge::UnifiedMessageV2& m, bool command) override;
    int frameSize(const Bytes& input) const override;
    bool consume(const Bytes& frame) override;
    bool polling() const override { return true; }
    void preparePoll(edge::UnifiedMessageV2& message) const override;
    iot::Protocol protocol() const override { return iot::Protocol::mitsubishi_mc; }
    std::string unknownWriteStatus() const override { return "unknown-result"; }
private:
    McConfig mc_;
};
}  // namespace mqmgateway::drivers

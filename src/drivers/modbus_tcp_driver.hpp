#pragma once
#include "tcp_driver.hpp"
namespace mqmgateway::drivers {
struct ModbusTcpConfig : TcpConfig {
    unsigned slave{1}, registerAddress{0};
};
class ModbusTcpDriver final : public TcpDriver {
public:
    ModbusTcpDriver(ModbusTcpConfig config, std::size_t capacity);
    std::string id() const override { return "modbus_tcp:" + mConfig.deviceId; }
    std::string defaultCommand() const override { return "modbus_tcp_write"; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2& m) const override {
        return edge::PointDefinition{m.deviceId, "holding-" + std::to_string(m.legacyAddress),
                                     edge::PointValueType::integer, "", true, m.legacyAddress};
    }
protected:
    bool valid(const edge::UnifiedMessageV2&) const override;
    Bytes encode(const edge::UnifiedMessageV2&, bool) override;
    int frameSize(const Bytes&) const override;
    bool consume(const Bytes&) override;
    bool polling() const override { return true; }
    void preparePoll(edge::UnifiedMessageV2& m) const override {
        m.legacyAddress = mModbus.registerAddress;
        m.legacySlave = static_cast<std::uint8_t>(mModbus.slave);
    }
    iot::Protocol protocol() const override { return iot::Protocol::modbus_tcp; }
private:
    ModbusTcpConfig mModbus;
    std::uint16_t mTransaction{0};
};
}

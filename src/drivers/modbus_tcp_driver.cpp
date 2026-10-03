#include "modbus_tcp_driver.hpp"
#include <stdexcept>
namespace mqmgateway::drivers {
ModbusTcpDriver::ModbusTcpDriver(ModbusTcpConfig config, std::size_t capacity)
    : TcpDriver(config, capacity), mModbus(std::move(config)) {
    if (mModbus.slave < 1 || mModbus.slave > 247 || mModbus.registerAddress > 65535)
        throw std::invalid_argument("invalid Modbus TCP slave/register");
}
bool ModbusTcpDriver::valid(const edge::UnifiedMessageV2& m) const {
    return m.legacySlave == mModbus.slave && m.legacyAddress <= 65535 && m.rawPayload.size() == 2;
}
TcpDriver::Bytes ModbusTcpDriver::encode(const edge::UnifiedMessageV2& m, bool command) {
    ++mTransaction;
    Bytes b{static_cast<uint8_t>(mTransaction >> 8), static_cast<uint8_t>(mTransaction), 0, 0, 0, 6,
            static_cast<uint8_t>(mModbus.slave), static_cast<uint8_t>(command ? 6 : 3),
            static_cast<uint8_t>(m.legacyAddress >> 8), static_cast<uint8_t>(m.legacyAddress)};
    if (command) b.insert(b.end(), m.rawPayload.begin(), m.rawPayload.end());
    else { b.push_back(0); b.push_back(1); }
    return b;
}
int ModbusTcpDriver::frameSize(const Bytes& b) const {
    if (b.size() < 6) return 0;
    const unsigned length = (b[4] << 8) | b[5];
    if (b[2] || b[3] || length < 2 || length > 254) return -1;
    return static_cast<int>(length + 6);
}
bool ModbusTcpDriver::consume(const Bytes& b) {
    if (!mPending || b.size() < 8 || b[0] != (mTransaction >> 8) || b[1] != (mTransaction & 255) ||
        b[6] != mModbus.slave || mPending->offset != mPending->wire.size()) return false;
    const auto function = mPending->command ? 6 : 3;
    if (b[7] == (function | 0x80) && b.size() == 9) {
        finish("error", "Modbus exception " + std::to_string(b[8])); return true;
    }
    if (b[7] != function) return false;
    if (mPending->command) {
        if (b != mPending->wire) return false;
        finish("ok", "Modbus TCP write confirmed");
    } else {
        if (b.size() != 11 || b[8] != 2) return false;
        emitTelemetry({b[9], b[10]}, mPending->message.legacyAddress);
        finish("ok", "");
    }
    return true;
}
}

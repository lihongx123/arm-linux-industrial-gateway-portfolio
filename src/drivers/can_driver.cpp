#include "can_driver.hpp"
#include <sys/epoll.h>
#include <stdexcept>

namespace mqmgateway::drivers {
CanDriver::CanDriver(std::string interfaceName, bool observe)
    : socket_(std::move(interfaceName), observe), observe_(observe) {}
bool CanDriver::acceptsDevice(const std::string& deviceId) const {
    // Unknown legacy CAN aliases remain accepted; configured competing devices
    // are explicitly bound in DeviceRegistry before discovery.
    return !deviceId.empty();
}
std::uint32_t CanDriver::desiredEvents() const { return EPOLLIN; }
void CanDriver::onReady(std::uint32_t events) {
    if (events & (EPOLLERR | EPOLLHUP)) throw std::runtime_error("CAN endpoint disconnected");
    if (!(events & EPOLLIN)) return;
    for (unsigned budget = 0; budget < 64; ++budget) {
        iot::UnifiedMessage message;
        if (!socket_.receiveReady(message)) break;
        if (emit_) emit_(edge::fromLegacy(message));
    }
}
bool CanDriver::submit(const edge::UnifiedMessageV2& command) {
    if (!socket_.isOpen() || !acceptsDevice(command.deviceId)) return false;
    const bool sent = socket_.send(edge::toLegacy(command));
    if (!sent) ++errors_;
    auto result = command;
    result.dataType = iot::DataType::status;
    result.direction = iot::Direction::northbound;
    result.quality = sent ? iot::Quality::good : iot::Quality::unavailable;
    result.status = sent ? "ok" : "error";
    result.detail = sent ? "CAN frame transmitted" : "CAN transmit failed";
    result.enqueuedAt = std::chrono::steady_clock::now();
    result.sourceTime = std::chrono::system_clock::now();
    if (emit_) emit_(std::move(result));
    return true;
}
void CanDriver::appendMetrics(std::ostream& out) const {
    out << ",\"can_errors\":" << errors_.load();
    if (observe_) {
        out << ",\"can_pipeline\":";
        socket_.writeMetrics(out);
    }
}
}  // namespace mqmgateway::drivers

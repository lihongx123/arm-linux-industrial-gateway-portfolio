#pragma once
#include "device_driver.hpp"
#include "termios_rtu_transport.hpp"
#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>

namespace mqmgateway::drivers {
struct RtuConfig {
    std::string device;
    unsigned baud{115200}, slave{1}, registerAddress{0}, pollMs{100}, responseMs{1000};
    serial::Rs485Config rs485;
};
class ModbusRtuDriver final : public edge::IDeviceDriver, public edge::IEventDrivenDriver {
public:
    ModbusRtuDriver(RtuConfig config, std::size_t capacity);
    std::string id() const override { return "modbus_rtu"; }
    void setMessageSink(Emit emit) override { emit_ = std::move(emit); }
    void start() override;
    void stop() noexcept override;
    bool submit(const edge::UnifiedMessageV2& command) override;
    bool acceptsDevice(const std::string& deviceId) const override;
    std::string defaultCommand() const override { return "modbus_write"; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2& m) const override {
        return edge::PointDefinition{m.deviceId, "holding-" + std::to_string(m.legacyAddress),
                                     edge::PointValueType::integer, "", true, m.legacyAddress};
    }
    int nativeHandle() const override { return serial_ ? serial_->nativeHandle() : -1; }
    std::uint32_t desiredEvents() const override;
    void setWake(std::function<void()> wake) override { wake_ = std::move(wake); }
    void onTick(std::chrono::steady_clock::time_point now) override;
    void onReady(std::uint32_t events) override;
    void appendMetrics(std::ostream& out) const override;
private:
    using Clock = std::chrono::steady_clock;
    RtuConfig config_;
    std::size_t capacity_;
    Emit emit_;
    std::function<void()> wake_;
    std::unique_ptr<serial::TermiosRtuTransport> serial_;
    std::mutex mutex_;
    std::deque<iot::UnifiedMessage> commands_;
    struct Pending {
        iot::UnifiedMessage message;
        serial::ByteBuffer request;
        std::size_t offset;
        Clock::time_point deadline;
        bool command;
    };
    std::optional<Pending> pending_;
    Clock::time_point nextPoll_{}, quietUntil_{};
    bool wantsWrite_{false};
    std::atomic<std::uint64_t> received_{0}, timeouts_{0}, unexpected_{0}, rejected_{0}, writes_{0};
    std::atomic<std::uint64_t> parserRejected_{0}, parserDiscarded_{0}, parserBuffered_{0};
    void flush();
    void received(const serial::ByteBuffer& frame);
    void finish(iot::Quality quality, const serial::ByteBuffer& response = {});
};
}  // namespace mqmgateway::drivers

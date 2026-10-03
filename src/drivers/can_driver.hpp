#pragma once
#include "device_driver.hpp"
#include "can_socket.hpp"
#include <atomic>

namespace mqmgateway::drivers {
class CanDriver final : public edge::IDeviceDriver, public edge::IEventDrivenDriver {
public:
    explicit CanDriver(std::string interfaceName, bool observe);
    std::string id() const override { return "can"; }
    void setMessageSink(Emit emit) override { emit_ = std::move(emit); }
    void start() override { socket_.open(); }
    void stop() noexcept override { socket_.close(); }
    bool submit(const edge::UnifiedMessageV2& command) override;
    bool acceptsDevice(const std::string& deviceId) const override;
    std::string defaultCommand() const override { return "can_tx"; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2& m) const override {
        return edge::PointDefinition{m.deviceId, "frame", edge::PointValueType::bytes, "", true, m.legacyAddress};
    }
    int nativeHandle() const override { return socket_.nativeHandle(); }
    std::uint32_t desiredEvents() const override;
    void onReady(std::uint32_t events) override;
    void appendMetrics(std::ostream& out) const override;
private:
    iot::CanSocket socket_;
    bool observe_;
    Emit emit_;
    std::atomic<std::uint64_t> errors_{0};
};
}  // namespace mqmgateway::drivers

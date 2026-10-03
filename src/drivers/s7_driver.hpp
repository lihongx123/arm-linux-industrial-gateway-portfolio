#pragma once
#include "device_driver.hpp"
#include <atomic>
#include <mutex>

namespace mqmgateway::drivers {
// Snap7-backed S7 TCP client. Scope: DB word (2 bytes), explicit rack/slot.
struct S7Config {
    std::string deviceId, pointId, address;
    unsigned port{102}, rack{0}, slot{2}, dbNumber{1}, byteOffset{0};
    unsigned intervalMs{1000};
    bool writable{false};
};

class S7Driver final : public edge::IDeviceDriver, public edge::IAcquisitionDriver {
public:
    explicit S7Driver(S7Config config);
    ~S7Driver() override { stop(); }
    std::string id() const override { return "s7:" + config_.deviceId; }
    std::string defaultCommand() const override { return config_.writable ? "s7_write" : ""; }
    bool acceptsDevice(const std::string& device) const override { return device == config_.deviceId; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2&) const override;
    void setMessageSink(Emit emit) override { emit_ = std::move(emit); }
    void start() override;
    void stop() noexcept override;
    bool submit(const edge::UnifiedMessageV2&) override;
    std::chrono::milliseconds interval() const override { return std::chrono::milliseconds(config_.intervalMs); }
    void acquire(std::chrono::steady_clock::time_point now) override;
    void appendMetrics(std::ostream&) const override;
private:
    bool connectLocked();
    void failLocked(unsigned code);
    S7Config config_;
    void* client_{nullptr};
    Emit emit_;
    mutable std::mutex mutex_;
    bool started_{false};
    std::atomic<bool> connected_{false};
    std::atomic<std::uint64_t> reads_{0}, writes_{0}, errors_{0}, reconnects_{0}, timeouts_{0}, consecutiveFailures_{0};
    std::atomic<std::int64_t> lastSuccessEpochMs_{0};
    std::atomic<unsigned> lastErrorCode_{0};
};
}  // namespace mqmgateway::drivers

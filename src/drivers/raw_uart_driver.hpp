#pragma once
#include "device_driver.hpp"
#include "serial_port.hpp"
#include <atomic>
#include <deque>
#include <mutex>
namespace mqmgateway::drivers {
struct RawUartConfig {
    std::string deviceId, pointId;
    serial::SerialConfig serial;
    bool delimiterMode{true}, writable{true};
    std::uint8_t delimiter{10};
    unsigned frameLength{0}, reconnectMs{250};
};
class RawUartDriver final : public edge::IDeviceDriver, public edge::IEventDrivenDriver {
public:
    RawUartDriver(RawUartConfig config, std::size_t capacity);
    std::string id() const override { return "uart:" + mConfig.deviceId; }
    std::string defaultCommand() const override { return mConfig.writable ? "write" : ""; }
    bool acceptsDevice(const std::string& id) const override { return id == mConfig.deviceId; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2&) const override {
        return edge::PointDefinition{mConfig.deviceId, mConfig.pointId, edge::PointValueType::bytes, "", mConfig.writable};
    }
    void setMessageSink(Emit emit) override { mEmit = std::move(emit); }
    void start() override;
    void stop() noexcept override;
    bool submit(const edge::UnifiedMessageV2&) override;
    int nativeHandle() const override { return mPort.fd(); }
    std::uint32_t desiredEvents() const override;
    void onReady(std::uint32_t events) override;
    void onTick(std::chrono::steady_clock::time_point now) override;
    void setWake(std::function<void()> wake) override { std::lock_guard<std::mutex> lock(mMutex); mWake = std::move(wake); }
    void appendMetrics(std::ostream&) const override;
private:
    using Bytes = std::vector<std::uint8_t>;
    RawUartConfig mConfig;
    serial::SerialPort mPort;
    const std::size_t mCapacity;
    Emit mEmit;
    std::function<void()> mWake;
    mutable std::mutex mMutex;
    std::deque<edge::UnifiedMessageV2> mCommands;
    Bytes mBuffer;
    bool mDroppingOversized{false};
    std::atomic<bool> mOnline{false};
    std::chrono::steady_clock::time_point mRetry{};
    std::atomic<std::uint64_t> mFrames{0}, mWrites{0}, mErrors{0}, mReopens{0}, mOversized{0};
    void emitFrame();
    void flush();
    void disconnect();
};
}

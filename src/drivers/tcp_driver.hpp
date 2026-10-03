#pragma once
#include "device_driver.hpp"
#include "../transport/tcp_transport.hpp"
#include <atomic>
#include <deque>
#include <mutex>
#include <optional>
namespace mqmgateway::drivers {
struct TcpConfig {
    std::string deviceId, address;
    unsigned port{0}, pollMs{100}, responseMs{1000}, reconnectMs{250};
};
// One nonblocking connection per device; shared transport/lifecycle, protocol-specific codecs.
class TcpDriver : public edge::IDeviceDriver, public edge::IEventDrivenDriver {
public:
    TcpDriver(TcpConfig config, std::size_t capacity);
    void setMessageSink(Emit emit) override { mEmit = std::move(emit); }
    void start() override;
    void stop() noexcept override;
    bool acceptsDevice(const std::string& device) const override { return device == mConfig.deviceId; }
    bool submit(const edge::UnifiedMessageV2& command) override;
    int nativeHandle() const override { return mTransport.fd(); }
    std::uint32_t desiredEvents() const override;
    void onReady(std::uint32_t events) override;
    void onTick(std::chrono::steady_clock::time_point now) override;
    void setWake(std::function<void()> wake) override { std::lock_guard<std::mutex> lock(mMutex); mWake = std::move(wake); }
    void appendMetrics(std::ostream& out) const override;
protected:
    using Bytes = std::vector<std::uint8_t>;
    using Clock = std::chrono::steady_clock;
    TcpConfig mConfig;
    struct Pending { edge::UnifiedMessageV2 message; Bytes wire; std::size_t offset{0}; Clock::time_point deadline; bool command; };
    std::optional<Pending> mPending;
    virtual bool valid(const edge::UnifiedMessageV2&) const = 0;
    virtual Bytes encode(const edge::UnifiedMessageV2&, bool command) = 0;
    // 0 incomplete, -1 malformed, positive complete frame length.
    virtual int frameSize(const Bytes&) const = 0;
    virtual bool consume(const Bytes&) = 0;
    virtual bool polling() const { return false; }
    virtual void preparePoll(edge::UnifiedMessageV2&) const {}
    virtual iot::Protocol protocol() const = 0;
    virtual std::string unknownWriteStatus() const { return "unavailable"; }
    void emitTelemetry(Bytes payload, unsigned address = 0);
    void finish(const std::string& status, const std::string& detail);
    void markSuccess();
    void markFailure(unsigned code);
private:
    transport::TcpTransport mTransport;
    Emit mEmit;
    std::function<void()> mWake;
    mutable std::mutex mMutex;
    std::deque<edge::UnifiedMessageV2> mCommands;
    const std::size_t mCapacity;
    Bytes mInput;
    bool mConnecting{false};
    std::atomic<bool> mOnline{false};
    Clock::time_point mRetry{}, mConnectDeadline{}, mNextPoll{};
    std::atomic<std::uint64_t> mConnections{0}, mDisconnects{0}, mTimeouts{0}, mMalformed{0}, mRejected{0};
    std::atomic<std::uint64_t> mReconnects{0}, mConsecutiveFailures{0};
    std::atomic<std::int64_t> mLastSuccessEpochMs{0};
    std::atomic<unsigned> mLastErrorCode{0}; // 0 none, 1 disconnect, 2 timeout, 3 protocol.
    void disconnect(unsigned reason = 1);
    void flush();
};
}

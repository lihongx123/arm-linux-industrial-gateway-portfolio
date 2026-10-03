#pragma once
#include "device_driver.hpp"
#include <atomic>
#include <memory>
#include <mutex>
namespace mqmgateway::board {
struct BoardConfig {
    std::string deviceId, pointId, path;
    unsigned intervalMs{100}, readLength{1};
    edge::PointValueType type{edge::PointValueType::bytes};
    double scale{1}, offset{0};
    bool writable{false};
};
class IBoardBackend {
public:
    virtual ~IBoardBackend() = default;
    virtual void open() = 0;
    virtual void close() noexcept = 0;
    virtual std::vector<std::uint8_t> read() = 0;
    virtual void write(const std::vector<std::uint8_t>&) = 0;
};
class BoardDriver : public edge::IDeviceDriver, public edge::IAcquisitionDriver {
public:
    BoardDriver(std::string kind, BoardConfig config, std::shared_ptr<IBoardBackend> backend);
    std::string id() const override { return mKind + ":" + mConfig.deviceId; }
    bool acceptsDevice(const std::string& id) const override { return id == mConfig.deviceId; }
    std::string defaultCommand() const override { return mConfig.writable ? "write" : ""; }
    std::optional<edge::PointDefinition> describePoint(const edge::UnifiedMessageV2&) const override;
    void setMessageSink(Emit emit) override { mEmit = std::move(emit); }
    void start() override;
    void stop() noexcept override;
    bool submit(const edge::UnifiedMessageV2&) override;
    std::chrono::milliseconds interval() const override { return std::chrono::milliseconds(mConfig.intervalMs); }
    void acquire(std::chrono::steady_clock::time_point) override;
    void appendMetrics(std::ostream&) const override;
private:
    std::string mKind;
    BoardConfig mConfig;
    std::shared_ptr<IBoardBackend> mBackend;
    Emit mEmit;
    mutable std::mutex mMutex;
    bool mStarted{false};
    std::atomic<bool> mHealthy{false};
    std::atomic<std::uint64_t> mReads{0}, mWrites{0}, mErrors{0}, mSamples{0};
};
}

#include "board_driver.hpp"
#include <cmath>
#include <stdexcept>
namespace mqmgateway::board {
BoardDriver::BoardDriver(std::string kind, BoardConfig config, std::shared_ptr<IBoardBackend> backend)
    : mKind(std::move(kind)), mConfig(std::move(config)), mBackend(std::move(backend)) {
    if (!mBackend || (mKind != "spi" && mKind != "i2c" && mKind != "gpio") ||
        mConfig.deviceId.empty() || mConfig.pointId.empty() || !mConfig.intervalMs ||
        !mConfig.readLength || mConfig.readLength > 4096 || !std::isfinite(mConfig.scale) || !std::isfinite(mConfig.offset))
        throw std::invalid_argument("invalid board driver config");
}
std::optional<edge::PointDefinition> BoardDriver::describePoint(const edge::UnifiedMessageV2&) const {
    return edge::PointDefinition{mConfig.deviceId, mConfig.pointId, mConfig.type, "", mConfig.writable, 0,
                                 mConfig.scale, mConfig.offset};
}
void BoardDriver::start() {
    std::lock_guard<std::mutex> lock(mMutex);
    mBackend->open(); mStarted = true; mHealthy = true;
}
void BoardDriver::stop() noexcept {
    std::lock_guard<std::mutex> lock(mMutex);
    mStarted = false; mHealthy = false; mBackend->close();
}
bool BoardDriver::submit(const edge::UnifiedMessageV2& command) {
    if (!mConfig.writable || command.rawPayload.empty() || command.rawPayload.size() > 4096) return false;
    bool ok = false;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mStarted) return false;
        try { mBackend->write(command.rawPayload); ++mWrites; mHealthy = true; ok = true; }
        catch (...) { ++mErrors; mHealthy = false; }
    }
    auto result = command;
    result.dataType = iot::DataType::status; result.direction = iot::Direction::northbound;
    result.status = ok ? "ok" : "error"; result.detail = ok ? "board write completed" : "board write failed";
    result.quality = ok ? iot::Quality::good : iot::Quality::unavailable;
    result.sourceTime = std::chrono::system_clock::now();
    if (mEmit) mEmit(std::move(result));
    return true;
}
void BoardDriver::acquire(std::chrono::steady_clock::time_point now) {
    std::vector<std::uint8_t> bytes;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (!mStarted) return;
        try { bytes = mBackend->read(); ++mReads; mHealthy = true; }
        catch (...) { ++mErrors; mHealthy = false; return; }
    }
    if (bytes.size() != mConfig.readLength) { ++mErrors; mHealthy = false; return; }
    edge::UnifiedMessageV2 m;
    m.deviceId = mConfig.deviceId; m.pointId = mConfig.pointId; m.driverId = id();
    m.rawPayload = std::move(bytes); m.sourceTime = std::chrono::system_clock::now(); m.enqueuedAt = now;
    if (mKind == "spi") m.legacyProtocol = iot::Protocol::spi;
    else if (mKind == "i2c") m.legacyProtocol = iot::Protocol::i2c;
    else m.legacyProtocol = iot::Protocol::gpio;
    if (mEmit) mEmit(std::move(m));
    ++mSamples;
}
void BoardDriver::appendMetrics(std::ostream& out) const {
    out << ",\"" << id() << "\":{\"reads\":" << mReads.load() << ",\"writes\":" << mWrites.load()
        << ",\"errors\":" << mErrors.load() << ",\"samples\":" << mSamples.load()
        << ",\"healthy\":" << (mHealthy ? "true" : "false") << '}';
}
}

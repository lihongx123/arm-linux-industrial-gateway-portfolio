#include "raw_uart_driver.hpp"
#include <cerrno>
#include <sys/epoll.h>
#include <unistd.h>
#include <stdexcept>
namespace mqmgateway::drivers {
RawUartDriver::RawUartDriver(RawUartConfig c, std::size_t capacity)
    : mConfig(std::move(c)), mPort(mConfig.serial), mCapacity(capacity) {
    if (mConfig.deviceId.empty() || mConfig.pointId.empty() || mConfig.serial.path.empty() ||
        !mConfig.reconnectMs || !capacity || (!mConfig.delimiterMode && (!mConfig.frameLength || mConfig.frameLength > 4096)))
        throw std::invalid_argument("invalid UART configuration");
}
void RawUartDriver::start() { mRetry = {}; }
void RawUartDriver::stop() noexcept {
    mOnline = false; mPort.close(); mBuffer.clear(); mDroppingOversized = false;
    std::lock_guard<std::mutex> lock(mMutex); mCommands.clear();
}
bool RawUartDriver::submit(const edge::UnifiedMessageV2& command) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mConfig.writable || !mOnline || command.rawPayload.empty() ||
        command.rawPayload.size() > 4096 || mCommands.size() >= mCapacity) return false;
    mCommands.push_back(command);
    if (mWake) mWake();
    return true;
}
std::uint32_t RawUartDriver::desiredEvents() const {
    std::lock_guard<std::mutex> lock(mMutex);
    return EPOLLIN | EPOLLRDHUP | (!mCommands.empty() ? EPOLLOUT : 0U);
}
void RawUartDriver::emitFrame() {
    if (mBuffer.empty()) return;
    edge::UnifiedMessageV2 m;
    m.deviceId = mConfig.deviceId; m.pointId = mConfig.pointId;
    m.legacyProtocol = iot::Protocol::uart;
    m.rawPayload = mBuffer; m.sourceTime = std::chrono::system_clock::now();
    m.enqueuedAt = std::chrono::steady_clock::now();
    if (mEmit) mEmit(std::move(m));
    ++mFrames; mBuffer.clear();
}
void RawUartDriver::disconnect() {
    mOnline = false; mPort.close(); mBuffer.clear(); mDroppingOversized = false; ++mErrors;
    mRetry = std::chrono::steady_clock::now() + std::chrono::milliseconds(mConfig.reconnectMs);
    std::deque<edge::UnifiedMessageV2> commands;
    { std::lock_guard<std::mutex> lock(mMutex); commands.swap(mCommands); }
    for (auto& m : commands) {
        m.dataType = iot::DataType::status; m.status = "unavailable"; m.detail = "UART disconnected; write not replayed";
        m.quality = iot::Quality::unavailable;
        if (mEmit) mEmit(std::move(m));
    }
}
void RawUartDriver::flush() {
    edge::UnifiedMessageV2 command;
    {
        std::lock_guard<std::mutex> lock(mMutex);
        if (mCommands.empty()) return;
        command = mCommands.front();
    }
    // One bounded command per reactor dispatch, no unbounded write spin.
    const auto n = ::write(mPort.fd(), command.rawPayload.data(), command.rawPayload.size());
    if (n < 0 && (errno == EAGAIN || errno == EINTR)) return;
    if (n < 0) { disconnect(); return; }
    if (n == 0 || static_cast<std::size_t>(n) < command.rawPayload.size()) {
        // Partial bytes may have reached the device; do not replay.
        disconnect(); return;
    }
    { std::lock_guard<std::mutex> lock(mMutex); mCommands.pop_front(); }
    ++mWrites; command.dataType = iot::DataType::status; command.direction = iot::Direction::northbound;
    command.status = "ok"; command.detail = "UART frame written to serial device";
    command.sourceTime = std::chrono::system_clock::now();
    if (mEmit) mEmit(std::move(command));
}
void RawUartDriver::onTick(std::chrono::steady_clock::time_point now) {
    if (mPort.fd() >= 0 || now < mRetry) return;
    mRetry = now + std::chrono::milliseconds(mConfig.reconnectMs);
    try { mPort.open(); mOnline = true; ++mReopens; }
    catch (...) { ++mErrors; }
}
void RawUartDriver::onReady(std::uint32_t events) {
    if (events & EPOLLIN) {
        for (unsigned budget = 0; budget < 16; ++budget) {
            std::uint8_t bytes[256];
            const auto n = ::read(mPort.fd(), bytes, sizeof(bytes));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n < 0) { disconnect(); return; }
            // VMIN=0/VTIME=0 terminals may return zero when drained, even when
            // the peer remains connected. HUP/ERR below handles real closure.
            if (n == 0) break;
            for (ssize_t i = 0; i < n; ++i) {
                if (mConfig.delimiterMode && bytes[i] == mConfig.delimiter) {
                    if (!mDroppingOversized) emitFrame();
                    mDroppingOversized = false;
                    continue;
                }
                if (mDroppingOversized) continue;
                mBuffer.push_back(bytes[i]);
                if (mBuffer.size() > 4096) {
                    mBuffer.clear(); ++mOversized;
                    mDroppingOversized = mConfig.delimiterMode;
                }
                else if (!mConfig.delimiterMode && mBuffer.size() == mConfig.frameLength) emitFrame();
            }
        }
    }
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) { disconnect(); return; }
    if (events & EPOLLOUT) flush();
}
void RawUartDriver::appendMetrics(std::ostream& out) const {
    out << ",\"" << id() << "\":{\"frames\":" << mFrames.load() << ",\"writes\":" << mWrites.load()
        << ",\"errors\":" << mErrors.load() << ",\"reopens\":" << mReopens.load()
        << ",\"oversized\":" << mOversized.load() << '}';
}
}

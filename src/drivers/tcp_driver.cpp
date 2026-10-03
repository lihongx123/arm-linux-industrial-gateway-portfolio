#include "tcp_driver.hpp"
#include <algorithm>
#include <arpa/inet.h>
#include <cerrno>
#include <stdexcept>
#include <sys/epoll.h>
namespace mqmgateway::drivers {
TcpDriver::TcpDriver(TcpConfig config, std::size_t capacity) : mConfig(std::move(config)), mCapacity(capacity) {
    in_addr address{};
    if (mConfig.deviceId.empty() || mConfig.deviceId.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_-") != std::string::npos ||
        inet_pton(AF_INET, mConfig.address.c_str(), &address) != 1 || !mConfig.port || mConfig.port > 65535 ||
        !mConfig.responseMs || !mConfig.reconnectMs || !mConfig.pollMs || !capacity)
        throw std::invalid_argument("invalid TCP device configuration (numeric IPv4 required)");
}
void TcpDriver::markSuccess() {
    mLastSuccessEpochMs = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    mConsecutiveFailures = 0;
    mLastErrorCode = 0;
}
void TcpDriver::markFailure(unsigned code) {
    ++mConsecutiveFailures;
    mLastErrorCode = code;
}
void TcpDriver::start() { mRetry = {}; mNextPoll = {}; }
void TcpDriver::stop() noexcept {
    mOnline = false;
    mTransport.close();
    std::lock_guard<std::mutex> lock(mMutex);
    mCommands.clear(); mPending.reset(); mInput.clear(); mConnecting = false;
}
bool TcpDriver::submit(const edge::UnifiedMessageV2& command) {
    std::lock_guard<std::mutex> lock(mMutex);
    if (!mOnline || !acceptsDevice(command.deviceId) || !valid(command) || mCommands.size() >= mCapacity) {
        ++mRejected; return false;
    }
    auto message = command;
    if (message.enqueuedAt == Clock::time_point{}) message.enqueuedAt = Clock::now();
    mCommands.push_back(std::move(message));
    if (mWake) mWake();
    return true;
}
std::uint32_t TcpDriver::desiredEvents() const {
    return EPOLLIN | EPOLLRDHUP | ((mConnecting || (mPending && mPending->offset < mPending->wire.size())) ? EPOLLOUT : 0U);
}
void TcpDriver::finish(const std::string& status, const std::string& detail) {
    if (!mPending) return;
    auto pending = std::move(*mPending); mPending.reset();
    if (!pending.command) return;
    auto& message = pending.message;
    message.dataType = iot::DataType::status;
    message.direction = iot::Direction::northbound;
    message.status = status; message.detail = detail;
    message.sourceTime = std::chrono::system_clock::now(); message.enqueuedAt = Clock::now();
    message.quality = (status == "ok" || status == "success") ? iot::Quality::good : iot::Quality::unavailable;
    if (mEmit) mEmit(std::move(message));
}
void TcpDriver::emitTelemetry(Bytes payload, unsigned address) {
    edge::UnifiedMessageV2 message;
    message.deviceId = mConfig.deviceId; message.legacyProtocol = protocol();
    message.legacyAddress = address;
    message.rawPayload = std::move(payload); message.sourceTime = std::chrono::system_clock::now();
    message.enqueuedAt = Clock::now();
    if (mEmit) mEmit(std::move(message));
}
void TcpDriver::disconnect(unsigned reason) {
    const bool uncertainWrite = mPending && mPending->command && mPending->offset > 0;
    markFailure(reason);
    mOnline = false; mConnecting = false; mTransport.close(); mInput.clear();
    ++mDisconnects; mRetry = Clock::now() + std::chrono::milliseconds(mConfig.reconnectMs);
    finish(uncertainWrite ? unknownWriteStatus() : "unavailable",
           uncertainWrite ? "TCP disconnected after dispatch; write outcome unknown; not replayed"
                          : "TCP disconnected before dispatch; not replayed");
    std::deque<edge::UnifiedMessageV2> queued;
    { std::lock_guard<std::mutex> lock(mMutex); queued.swap(mCommands); }
    for (auto& message : queued) {
        mPending = Pending{std::move(message), {}, 0, {}, true};
        finish("unavailable", "TCP disconnected before dispatch; not replayed");
    }
}
void TcpDriver::flush() {
    if (!mPending || !mOnline) return;
    while (mPending->offset < mPending->wire.size()) {
        auto& p = *mPending;
        const auto n = mTransport.write(p.wire.data() + p.offset, p.wire.size() - p.offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) return;
        if (n <= 0) { disconnect(); return; }
        p.offset += static_cast<std::size_t>(n);
    }
    if (!polling()) finish("ok", "TCP frame sent; not an application acknowledgement");
}
void TcpDriver::onTick(Clock::time_point now) {
    if (mTransport.fd() < 0) {
        if (now < mRetry) return;
        mRetry = now + std::chrono::milliseconds(mConfig.reconnectMs);
        if (mTransport.connect(mConfig.address, mConfig.port)) {
            mConnecting = true;
            mConnectDeadline = now + std::chrono::milliseconds(mConfig.responseMs);
        } else markFailure(1);
        return;
    }
    if (mConnecting) { if (now >= mConnectDeadline) { ++mTimeouts; disconnect(2); } return; }
    if (mPending && now >= mPending->deadline) {
        ++mTimeouts; finish("timeout", "TCP response deadline; write not replayed"); disconnect(2); return;
    }
    if (mPending || !mOnline) return;
    std::optional<edge::UnifiedMessageV2> command;
    { std::lock_guard<std::mutex> lock(mMutex);
      if (!mCommands.empty()) { command = std::move(mCommands.front()); mCommands.pop_front(); } }
    if (!command && (!polling() || now < mNextPoll)) return;
    edge::UnifiedMessageV2 message;
    if (command) message = std::move(*command);
    else {
        message.deviceId = mConfig.deviceId; message.legacyProtocol = protocol();
        preparePoll(message);
        mNextPoll = now + std::chrono::milliseconds(mConfig.pollMs);
    }
    auto deadline = now + std::chrono::milliseconds(mConfig.responseMs);
    if (command) deadline = std::min(deadline, message.enqueuedAt + message.timeout);
    mPending = Pending{message, {}, 0, deadline, bool(command)};
    if (deadline <= now) { ++mTimeouts; finish("timeout", "TCP command expired before dispatch"); return; }
    mPending->wire = encode(message, bool(command));
    flush();
}
void TcpDriver::onReady(std::uint32_t events) {
    if (mConnecting) {
        if (!mTransport.finishConnect()) { disconnect(); return; }
        mConnecting = false; mOnline = true;
        if (++mConnections > 1) ++mReconnects;
    }
    if (events & EPOLLIN) {
        for (unsigned budget = 0; budget < 16; ++budget) {
            std::uint8_t buffer[4096];
            const auto n = mTransport.read(buffer, sizeof(buffer));
            if (n < 0 && errno == EINTR) continue;
            if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;
            if (n <= 0) { disconnect(); return; }
            mInput.insert(mInput.end(), buffer, buffer + n);
            while (!mInput.empty()) {
                const int size = frameSize(mInput);
                if (size < 0 || mInput.size() > 8192) { ++mMalformed; disconnect(3); return; }
                if (!size || mInput.size() < static_cast<std::size_t>(size)) break;
                Bytes frame(mInput.begin(), mInput.begin() + size);
                mInput.erase(mInput.begin(), mInput.begin() + size);
                if (!consume(frame)) { ++mMalformed; disconnect(3); return; }
            }
        }
    }
    if (events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) { disconnect(); return; }
    if (events & EPOLLOUT) flush();
}
void TcpDriver::appendMetrics(std::ostream& out) const {
    out << ",\"" << id() << "\":{\"connections\":" << mConnections.load()
        << ",\"disconnects\":" << mDisconnects.load() << ",\"timeouts\":" << mTimeouts.load()
        << ",\"malformed\":" << mMalformed.load() << ",\"rejected\":" << mRejected.load()
        << ",\"connected\":" << (mOnline.load() ? "true" : "false")
        << ",\"last_success_epoch_ms\":" << mLastSuccessEpochMs.load()
        << ",\"last_error_code\":" << mLastErrorCode.load()
        << ",\"consecutive_failures\":" << mConsecutiveFailures.load()
        << ",\"reconnects\":" << mReconnects.load() << '}';
}
}

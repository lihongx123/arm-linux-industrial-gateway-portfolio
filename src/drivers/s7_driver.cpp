#include "s7_driver.hpp"
#include <snap7.h>
#include <stdexcept>

namespace mqmgateway::drivers {
S7Driver::S7Driver(S7Config config) : config_(std::move(config)) {
    if (config_.deviceId.empty() || config_.pointId.empty() || config_.address.empty() ||
        config_.port == 0 || config_.port > 65535 || config_.rack > 7 || config_.slot > 31 ||
        config_.dbNumber == 0 || config_.dbNumber > 65535 ||
        config_.byteOffset > 65533 || !config_.intervalMs)
        throw std::invalid_argument("invalid S7 DB word configuration");
}
std::optional<edge::PointDefinition> S7Driver::describePoint(const edge::UnifiedMessageV2& message) const {
    if (message.deviceId != config_.deviceId && !message.deviceId.empty()) return std::nullopt;
    if (!message.pointId.empty() && message.pointId != config_.pointId) return std::nullopt;
    return edge::PointDefinition{config_.deviceId, config_.pointId, edge::PointValueType::integer,
                                 "", config_.writable, config_.byteOffset};
}
void S7Driver::start() {
    std::lock_guard<std::mutex> lock(mutex_);
    if (started_) return;
    client_ = new TS7Client();
    auto port = static_cast<word>(config_.port);
    if (static_cast<TS7Client*>(client_)->SetParam(p_u16_RemotePort, &port) != 0) {
        delete static_cast<TS7Client*>(client_); client_ = nullptr;
        throw std::runtime_error("Snap7 remote port configuration failed");
    }
    started_ = true;
}
void S7Driver::stop() noexcept {
    std::lock_guard<std::mutex> lock(mutex_);
    started_ = false; connected_ = false;
    if (client_) { static_cast<TS7Client*>(client_)->Disconnect(); delete static_cast<TS7Client*>(client_); client_ = nullptr; }
}
bool S7Driver::connectLocked() {
    if (!started_ || !client_) return false;
    if (connected_) return true;
    const auto code = static_cast<TS7Client*>(client_)->ConnectTo(config_.address.c_str(), config_.rack, config_.slot);
    if (code != 0) { failLocked(static_cast<unsigned>(code)); return false; }
    connected_ = true; ++reconnects_; return true;
}
void S7Driver::failLocked(unsigned code) {
    ++errors_; ++consecutiveFailures_; lastErrorCode_ = code; connected_ = false;
    if ((code & 0xFFF00000U) == errCliJobTimeout) ++timeouts_;
    if (client_) static_cast<TS7Client*>(client_)->Disconnect();
}
void S7Driver::acquire(std::chrono::steady_clock::time_point now) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!connectLocked()) return;
    std::uint8_t bytes[2]{};
    const auto code = static_cast<TS7Client*>(client_)->DBRead(config_.dbNumber, config_.byteOffset, 2, bytes);
    if (code != 0) { failLocked(static_cast<unsigned>(code)); return; }
    edge::UnifiedMessageV2 message;
    message.deviceId = config_.deviceId; message.pointId = config_.pointId; message.driverId = id();
    message.legacyProtocol = iot::Protocol::siemens_s7; message.rawPayload = {bytes[0], bytes[1]};
    message.sourceDescriptor = "DB" + std::to_string(config_.dbNumber) + ".DBW" + std::to_string(config_.byteOffset);
    message.sourceTime = std::chrono::system_clock::now(); message.enqueuedAt = now;
    ++reads_; consecutiveFailures_ = 0; lastErrorCode_ = 0;
    lastSuccessEpochMs_ = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    if (emit_) emit_(std::move(message));
}
bool S7Driver::submit(const edge::UnifiedMessageV2& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!config_.writable || !started_ || command.deviceId != config_.deviceId ||
        (!command.pointId.empty() && command.pointId != config_.pointId) || command.rawPayload.size() != 2 || !connectLocked()) return false;
    std::uint8_t bytes[2] = {command.rawPayload[0], command.rawPayload[1]};
    const auto code = static_cast<TS7Client*>(client_)->DBWrite(config_.dbNumber, config_.byteOffset, 2, bytes);
    auto result = command; result.dataType = iot::DataType::status; result.direction = iot::Direction::northbound;
    result.sourceDescriptor = "DB" + std::to_string(config_.dbNumber) + ".DBW" + std::to_string(config_.byteOffset);
    result.sourceTime = std::chrono::system_clock::now();
    if (code == 0) {
        ++writes_; consecutiveFailures_ = 0; lastErrorCode_ = 0;
        result.status = "success"; result.detail = "S7 DB word write acknowledged"; result.quality = iot::Quality::good;
    } else { failLocked(static_cast<unsigned>(code));
        result.status = (static_cast<unsigned>(code) & 0xFFF00000U) == errCliJobTimeout ? "timeout" : "failed";
        result.detail = "S7 error " + std::to_string(code); result.quality = iot::Quality::unavailable; }
    if (emit_) emit_(std::move(result));
    return true;
}
void S7Driver::appendMetrics(std::ostream& out) const {
    out << ",\"" << id() << "\":{\"connected\":" << (connected_ ? "true" : "false")
        << ",\"reads\":" << reads_.load() << ",\"writes\":" << writes_.load()
        << ",\"errors\":" << errors_.load() << ",\"reconnects\":" << reconnects_.load()
        << ",\"timeouts\":" << timeouts_.load()
        << ",\"consecutive_failures\":" << consecutiveFailures_.load()
        << ",\"last_success_epoch_ms\":" << lastSuccessEpochMs_.load()
        << ",\"last_error_code\":" << lastErrorCode_.load() << '}';
}
}  // namespace mqmgateway::drivers

#include "modbus_rtu_driver.hpp"
#include <algorithm>
#include <cerrno>
#include <stdexcept>
#include <sys/epoll.h>
#include <unistd.h>

namespace mqmgateway::drivers {
ModbusRtuDriver::ModbusRtuDriver(RtuConfig config, std::size_t capacity)
    : config_(std::move(config)), capacity_(capacity) {
    if (config_.slave < 1 || config_.slave > 247 || config_.registerAddress > 65535 ||
        !config_.pollMs || !config_.responseMs || !capacity_) throw std::invalid_argument("invalid RTU configuration");
}
bool ModbusRtuDriver::acceptsDevice(const std::string& deviceId) const {
    return !config_.device.empty() && deviceId == "rtu-" + std::to_string(config_.slave);
}
void ModbusRtuDriver::start() {
    if (config_.device.empty()) return;
    serial_ = std::make_unique<serial::TermiosRtuTransport>(config_.device, config_.baud, config_.rs485);
    serial_->open();
    nextPoll_ = Clock::now() + std::chrono::milliseconds(config_.pollMs);
    quietUntil_ = {};
}
void ModbusRtuDriver::stop() noexcept {
    if (serial_) serial_->close();
    std::lock_guard<std::mutex> lock(mutex_);
    commands_.clear();
    pending_.reset();
    wantsWrite_ = false;
}
bool ModbusRtuDriver::submit(const edge::UnifiedMessageV2& command) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!serial_ || !serial_->isOpen() || !acceptsDevice(command.deviceId) ||
        command.legacySlave != config_.slave || command.legacyAddress > 65535 ||
        command.rawPayload.size() != 2 || commands_.size() >= capacity_) {
        ++rejected_;
        return false;
    }
    commands_.push_back(edge::toLegacy(command));
    if (wake_) wake_();
    return true;
}
std::uint32_t ModbusRtuDriver::desiredEvents() const { return EPOLLIN | (wantsWrite_ ? EPOLLOUT : 0U); }
void ModbusRtuDriver::finish(iot::Quality quality, const serial::ByteBuffer& response) {
    if (!pending_) return;
    auto message = pending_->message;
    const bool command = pending_->command;
    pending_.reset();
    wantsWrite_ = false;
    quietUntil_ = Clock::now() + std::chrono::milliseconds(2);
    message.direction = iot::Direction::northbound;
    message.quality = quality;
    message.timestamp = std::chrono::system_clock::now();
    message.enqueuedAt = Clock::now();
    message.dataType = command ? iot::DataType::status : iot::DataType::telemetry;
    if (command && quality == iot::Quality::good) ++writes_;
    if (!response.empty()) message.payload = response;
    if (command || quality == iot::Quality::good) {
        auto result = edge::fromLegacy(message);
        if (!command && response.size() == 7 && response[1] == 3 && response[2] == 2)
            result.pointRawPayload = {response[3], response[4]};
        if (command) {
            result.status = quality == iot::Quality::good ? "ok" : iot::toString(quality);
            result.detail = "RTU transaction response";
        }
        if (emit_) emit_(std::move(result));
    }
}
void ModbusRtuDriver::flush() {
    if (!pending_) return;
    auto& p = *pending_;
    while (p.offset < p.request.size()) {
        const auto n = ::write(serial_->nativeHandle(), p.request.data() + p.offset, p.request.size() - p.offset);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0 && errno == EAGAIN) { wantsWrite_ = true; return; }
        if (n <= 0) { finish(iot::Quality::unavailable); return; }
        p.offset += static_cast<std::size_t>(n);
    }
    wantsWrite_ = false;
}
void ModbusRtuDriver::onTick(Clock::time_point now) {
    if (!serial_ || !serial_->isOpen()) return;
    if (pending_ && now >= pending_->deadline) {
        ++timeouts_;
        finish(iot::Quality::timeout);
        serial_->discardInput();
        quietUntil_ = now + std::chrono::milliseconds(config_.responseMs);
    }
    if (pending_ || now < quietUntil_) return;
    std::optional<iot::UnifiedMessage> command;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!commands_.empty()) { command = std::move(commands_.front()); commands_.pop_front(); }
    }
    iot::UnifiedMessage message;
    const bool isCommand = bool(command);
    if (command) message = std::move(*command);
    else {
        if (now < nextPoll_) return;
        nextPoll_ = now + std::chrono::milliseconds(config_.pollMs);
        message.protocol = iot::Protocol::modbus_rtu;
        message.deviceId = "rtu-" + std::to_string(config_.slave);
        message.slave = static_cast<std::uint8_t>(config_.slave);
        message.address = config_.registerAddress;
        message.timeout = std::chrono::milliseconds(config_.responseMs);
    }
    serial::ByteBuffer bytes{message.slave, static_cast<std::uint8_t>(isCommand ? 6 : 3),
        static_cast<std::uint8_t>(message.address >> 8U), static_cast<std::uint8_t>(message.address)};
    if (isCommand) bytes.insert(bytes.end(), message.payload.begin(), message.payload.end());
    else { bytes.push_back(0); bytes.push_back(1); }
    const auto deadline = std::min(now + std::chrono::milliseconds(config_.responseMs), message.enqueuedAt + message.timeout);
    pending_ = Pending{message, serial::RtuFrameParser::appendCrc(std::move(bytes)), 0, deadline, isCommand};
    if (now >= deadline) { ++timeouts_; finish(iot::Quality::timeout); return; }
    flush();
}
void ModbusRtuDriver::received(const serial::ByteBuffer& frame) {
    if (!pending_ || frame.size() < 2 || frame[0] != pending_->message.slave ||
        pending_->offset != pending_->request.size()) { ++unexpected_; return; }
    const unsigned function = pending_->command ? 6 : 3;
    if (frame[1] == (function | 0x80U) && frame.size() == 5) { finish(iot::Quality::invalid, frame); return; }
    const bool valid = pending_->command ? frame == pending_->request : frame.size() == 7 && frame[1] == 3 && frame[2] == 2;
    if (!valid) { ++unexpected_; return; }
    ++received_;
    finish(iot::Quality::good, frame);
}
void ModbusRtuDriver::onReady(std::uint32_t events) {
    if (events & EPOLLIN) {
        for (unsigned budget = 0; budget < 16; ++budget) {
            const auto before = serial_->parserMetrics().bytesReceived;
            for (const auto& frame : serial_->readAvailable()) received(frame);
            const auto& stats = serial_->parserMetrics();
            parserRejected_ = stats.crcCandidatesRejected;
            parserDiscarded_ = stats.bytesDiscarded;
            parserBuffered_ = stats.bufferedBytes;
            if (stats.bytesReceived == before) break;
        }
    }
    if (events & EPOLLOUT) flush();
    if (events & (EPOLLERR | EPOLLHUP)) throw std::runtime_error("RTU endpoint disconnected");
}
void ModbusRtuDriver::appendMetrics(std::ostream& out) const {
    out << ",\"rtu\":{\"responses\":" << received_.load() << ",\"timeouts\":" << timeouts_.load()
        << ",\"unexpected\":" << unexpected_.load() << ",\"rejected\":" << rejected_.load()
        << ",\"writes_confirmed\":" << writes_.load() << ",\"crc_candidates_rejected\":" << parserRejected_.load()
        << ",\"bytes_discarded\":" << parserDiscarded_.load() << ",\"buffered_bytes\":" << parserBuffered_.load() << '}';
}
}  // namespace mqmgateway::drivers

#include "termios_rtu_transport.hpp"

#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <stdexcept>
#include <termios.h>
#include <unistd.h>

#include <algorithm>

namespace mqmgateway::serial {

namespace {
constexpr std::size_t minimumFrameLength = 5;
constexpr std::size_t maximumModbusFrameLength = 256;

std::runtime_error systemError(const std::string& operation) {
    return std::runtime_error(operation + ": " + std::strerror(errno));
}
}  // namespace

RtuFrameParser::RtuFrameParser(const std::size_t maximumBufferBytes)
    : maximumBufferBytes_(std::max(maximumBufferBytes, maximumModbusFrameLength)) {}

std::uint16_t RtuFrameParser::crc16(const std::uint8_t* data, const std::size_t size) noexcept {
    std::uint16_t crc = 0xffff;
    for (std::size_t index = 0; index < size; ++index) {
        crc ^= data[index];
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? static_cast<std::uint16_t>((crc >> 1U) ^ 0xa001U)
                                   : static_cast<std::uint16_t>(crc >> 1U);
        }
    }
    return crc;
}

ByteBuffer RtuFrameParser::appendCrc(ByteBuffer payload) {
    const auto crc = crc16(payload.data(), payload.size());
    payload.push_back(static_cast<std::uint8_t>(crc & 0xffU));
    payload.push_back(static_cast<std::uint8_t>((crc >> 8U) & 0xffU));
    return payload;
}

std::vector<std::size_t> RtuFrameParser::candidateLengths(const std::size_t offset) const {
    if (buffer_.size() < offset + 2) return {};
    const auto function = buffer_[offset + 1];
    if ((function & 0x80U) != 0U) return {5};
    switch (function) {
        case 1:
        case 2:
        case 3:
        case 4: {
            std::vector<std::size_t> lengths{8};
            if (buffer_.size() >= offset + 3) {
                const auto responseLength = static_cast<std::size_t>(buffer_[offset + 2]) + 5;
                if (responseLength >= minimumFrameLength && responseLength <= maximumModbusFrameLength) {
                    lengths.push_back(responseLength);
                }
            }
            return lengths;
        }
        case 5:
        case 6:
            return {8};
        case 15:
        case 16: {
            std::vector<std::size_t> lengths{8};
            if (buffer_.size() >= offset + 7) {
                const auto requestLength = static_cast<std::size_t>(buffer_[offset + 6]) + 9;
                if (requestLength >= 9 && requestLength <= maximumModbusFrameLength) {
                    lengths.push_back(requestLength);
                }
            }
            return lengths;
        }
        default:
            return {};
    }
}

bool RtuFrameParser::validCrc(const std::size_t offset, const std::size_t length) const noexcept {
    if (length < 3 || offset + length > buffer_.size()) return false;
    const auto expected = crc16(buffer_.data() + offset, length - 2);
    const auto received = static_cast<std::uint16_t>(buffer_[offset + length - 2]) |
        static_cast<std::uint16_t>(buffer_[offset + length - 1] << 8U);
    return expected == received;
}

void RtuFrameParser::discardPrefix(const std::size_t count) {
    if (count == 0) return;
    metrics_.bytesDiscarded += count;
    ++metrics_.resyncEvents;
    buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(count));
}

std::vector<ByteBuffer> RtuFrameParser::feed(const ByteBuffer& data) {
    return feed(data.data(), data.size());
}

std::vector<ByteBuffer> RtuFrameParser::feed(const std::uint8_t* data, const std::size_t size) {
    std::vector<ByteBuffer> frames;
    metrics_.bytesReceived += size;
    if (!buffer_.empty() && size != 0) ++metrics_.fragmentedFeeds;
    buffer_.insert(buffer_.end(), data, data + size);

    while (buffer_.size() >= minimumFrameLength) {
        bool found = false;
        std::size_t foundOffset = 0;
        std::size_t foundLength = 0;
        std::uint64_t rejectedThisPass = 0;

        for (std::size_t offset = 0; offset + minimumFrameLength <= buffer_.size(); ++offset) {
            for (const auto length : candidateLengths(offset)) {
                if (offset + length > buffer_.size()) continue;
                if (validCrc(offset, length)) {
                    found = true;
                    foundOffset = offset;
                    foundLength = length;
                    break;
                }
                ++rejectedThisPass;
            }
            if (found) break;
        }

        if (found) {
            metrics_.crcCandidatesRejected += rejectedThisPass;
            discardPrefix(foundOffset);
            frames.emplace_back(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(foundLength));
            buffer_.erase(buffer_.begin(), buffer_.begin() + static_cast<std::ptrdiff_t>(foundLength));
            ++metrics_.framesAccepted;
            continue;
        }

        if (buffer_.size() > maximumBufferBytes_) {
            metrics_.crcCandidatesRejected += rejectedThisPass;
            discardPrefix(buffer_.size() - maximumModbusFrameLength);
            continue;
        }
        break;
    }
    metrics_.bufferedBytes = buffer_.size();
    return frames;
}

const RtuParserMetrics& RtuFrameParser::metrics() const noexcept { return metrics_; }

void RtuFrameParser::reset() noexcept {
    buffer_.clear();
    metrics_ = {};
}

void RtuFrameParser::discardBuffered() noexcept {
    discardPrefix(buffer_.size());
    metrics_.bufferedBytes = 0;
}

TermiosRtuTransport::TermiosRtuTransport(std::string device, const unsigned int baudRate, Rs485Config rs485)
    : port_(SerialConfig{std::move(device), baudRate, 8, 1, 'N', rs485}) {}

TermiosRtuTransport::~TermiosRtuTransport() { close(); }

void TermiosRtuTransport::open() { port_.open(); }
void TermiosRtuTransport::close() noexcept { port_.close(); }
bool TermiosRtuTransport::isOpen() const noexcept { return port_.fd() >= 0; }

void TermiosRtuTransport::writeFrame(const ByteBuffer& frame, const std::chrono::milliseconds timeout) {
    if (!isOpen()) throw std::logic_error("serial transport is not open");
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    std::size_t written = 0;
    while (written < frame.size()) {
        const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(
            deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) throw std::runtime_error("serial write timeout");
        pollfd descriptor{port_.fd(), POLLOUT, 0};
        const auto result = ::poll(&descriptor, 1, static_cast<int>(remaining.count()));
        if (result < 0 && errno == EINTR) continue;
        if (result <= 0) throw result == 0 ? std::runtime_error("serial write timeout") : systemError("poll write");
        const auto count = ::write(port_.fd(), frame.data() + written, frame.size() - written);
        if (count < 0 && (errno == EAGAIN || errno == EINTR)) continue;
        if (count < 0) throw systemError("serial write");
        written += static_cast<std::size_t>(count);
    }
    if (tcdrain(port_.fd()) != 0) throw systemError("tcdrain");
}

std::vector<ByteBuffer> TermiosRtuTransport::readFrames(const std::chrono::milliseconds timeout) {
    if (!isOpen()) throw std::logic_error("serial transport is not open");
    pollfd descriptor{port_.fd(), POLLIN, 0};
    int result;
    do {
        result = ::poll(&descriptor, 1, static_cast<int>(timeout.count()));
    } while (result < 0 && errno == EINTR);
    if (result < 0) throw systemError("poll read");
    if (result == 0) return {};

    return readAvailable();
}

std::vector<ByteBuffer> TermiosRtuTransport::readAvailable() {
    ByteBuffer bytes(512);
    const auto count = ::read(port_.fd(), bytes.data(), bytes.size());
    if (count < 0 && (errno == EAGAIN || errno == EINTR)) return {};
    if (count < 0) throw systemError("serial read");
    return parser_.feed(bytes.data(), static_cast<std::size_t>(count));
}

void TermiosRtuTransport::discardInput() {
    tcflush(port_.fd(), TCIFLUSH);
    parser_.discardBuffered();
}

const RtuParserMetrics& TermiosRtuTransport::parserMetrics() const noexcept { return parser_.metrics(); }

}  // namespace mqmgateway::serial

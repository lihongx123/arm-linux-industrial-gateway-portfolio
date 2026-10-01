#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace mqmgateway::serial {

using ByteBuffer = std::vector<std::uint8_t>;

struct RtuParserMetrics {
    std::uint64_t bytesReceived = 0;
    std::uint64_t framesAccepted = 0;
    std::uint64_t crcCandidatesRejected = 0;
    std::uint64_t bytesDiscarded = 0;
    std::uint64_t resyncEvents = 0;
    std::uint64_t fragmentedFeeds = 0;
    std::size_t bufferedBytes = 0;
};

class RtuFrameParser {
public:
    explicit RtuFrameParser(std::size_t maximumBufferBytes = 1024);

    std::vector<ByteBuffer> feed(const std::uint8_t* data, std::size_t size);
    std::vector<ByteBuffer> feed(const ByteBuffer& data);
    const RtuParserMetrics& metrics() const noexcept;
    void reset() noexcept;
    void discardBuffered() noexcept;

    static std::uint16_t crc16(const std::uint8_t* data, std::size_t size) noexcept;
    static ByteBuffer appendCrc(ByteBuffer payload);

private:
    std::vector<std::size_t> candidateLengths(std::size_t offset) const;
    bool validCrc(std::size_t offset, std::size_t length) const noexcept;
    void discardPrefix(std::size_t count);

    std::size_t maximumBufferBytes_;
    ByteBuffer buffer_;
    RtuParserMetrics metrics_;
};

class TermiosRtuTransport {
public:
    TermiosRtuTransport(std::string device, unsigned int baudRate = 115200);
    ~TermiosRtuTransport();

    TermiosRtuTransport(const TermiosRtuTransport&) = delete;
    TermiosRtuTransport& operator=(const TermiosRtuTransport&) = delete;

    void open();
    void close() noexcept;
    bool isOpen() const noexcept;
    int nativeHandle() const noexcept { return descriptor_; }
    std::vector<ByteBuffer> readAvailable();
    void discardInput();
    void writeFrame(const ByteBuffer& frame, std::chrono::milliseconds timeout);
    std::vector<ByteBuffer> readFrames(std::chrono::milliseconds timeout);
    const RtuParserMetrics& parserMetrics() const noexcept;

private:
    static unsigned int baudConstant(unsigned int baudRate);

    std::string device_;
    unsigned int baudRate_;
    int descriptor_ = -1;
    RtuFrameParser parser_;
};

}  // namespace mqmgateway::serial

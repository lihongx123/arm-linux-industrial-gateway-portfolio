#pragma once
#include "can_socket.hpp"
#include "termios_rtu_transport.hpp"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
namespace mqmgateway::iot {
struct RtuConfig {
    std::string device;
    unsigned baud{115200}, slave{1}, registerAddress{0}, pollMs{100}, responseMs{1000};
};
class SouthboundReactor {
public:
    using Emit = std::function<void(UnifiedMessage)>;
    SouthboundReactor(CanSocket&, RtuConfig, std::size_t capacity, Emit);
    ~SouthboundReactor();
    void run(const std::atomic<bool>& running);
    bool submit(UnifiedMessage command);
    void wake();
    void writeMetrics(std::ostream&) const;
private:
    using Clock = std::chrono::steady_clock;
    CanSocket& can_;
    RtuConfig config_;
    Emit emit_;
    int epoll_{-1}, wake_{-1};
    std::unique_ptr<serial::TermiosRtuTransport> serial_;
    std::mutex mutex_;
    std::deque<UnifiedMessage> commands_;
    std::size_t capacity_;
    struct Pending { UnifiedMessage message; serial::ByteBuffer request; std::size_t offset{0}; Clock::time_point deadline; bool command; };
    std::optional<Pending> pending_;
    Clock::time_point nextPoll_{}, quietUntil_{};
    std::atomic<uint64_t> rtuReceived_{0}, rtuTimeouts_{0}, rtuUnexpected_{0}, rtuRejected_{0}, rtuWrites_{0};
    std::atomic<uint64_t> parserRejected_{0}, parserDiscarded_{0}, parserBuffered_{0};
    void updateSerial(bool writable);
    void tick();
    void flush();
    void received(const serial::ByteBuffer&);
    void finish(Quality, const serial::ByteBuffer& response = {});
};
}

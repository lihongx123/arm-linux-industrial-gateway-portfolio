#include "termios_rtu_transport.hpp"

#include <fcntl.h>
#include <pty.h>
#include <unistd.h>

#include <chrono>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>

using mqmgateway::serial::ByteBuffer;
using mqmgateway::serial::RtuFrameParser;
using mqmgateway::serial::TermiosRtuTransport;

namespace {
int checks = 0;

void require(const bool condition, const std::string& message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}

ByteBuffer request(const std::uint8_t unit, const std::uint16_t address) {
    return RtuFrameParser::appendCrc({unit, 3, static_cast<std::uint8_t>(address >> 8U),
        static_cast<std::uint8_t>(address & 0xffU), 0, 2});
}

void parserScenarios() {
    RtuFrameParser parser(256);
    const auto first = request(1, 0x10);
    const auto second = request(2, 0x20);

    auto frames = parser.feed(first.data(), 3);
    require(frames.empty(), "fragment prefix must not emit a frame");
    frames = parser.feed(first.data() + 3, first.size() - 3);
    require(frames.size() == 1 && frames.front() == first, "fragmented frame reassembly failed");

    ByteBuffer sticky = first;
    sticky.insert(sticky.end(), second.begin(), second.end());
    frames = parser.feed(sticky);
    require(frames.size() == 2 && frames[0] == first && frames[1] == second, "sticky-frame split failed");

    ByteBuffer noisy{0xff, 0x00, 0x7e, 0x7e, 0x55};
    noisy.insert(noisy.end(), first.begin(), first.end());
    frames = parser.feed(noisy);
    require(frames.size() == 1 && frames.front() == first, "noise-prefix resynchronization failed");

    auto bad = first;
    bad.back() ^= 0x5a;
    ByteBuffer damaged;
    for (int repeat = 0; repeat < 32; ++repeat) damaged.insert(damaged.end(), bad.begin(), bad.end());
    damaged.insert(damaged.end(), second.begin(), second.end());
    frames = parser.feed(damaged);
    require(frames.size() == 1 && frames.front() == second, "continuous bad-frame recovery failed");
    require(parser.metrics().bytesDiscarded >= damaged.size() - second.size(), "discard metrics missing");
    require(parser.metrics().resyncEvents > 0, "resync metrics missing");
    require(parser.metrics().fragmentedFeeds > 0, "fragment metrics missing");
}

void termiosScenario() {
    int master = -1;
    int slave = -1;
    char slaveName[128]{};
    if (openpty(&master, &slave, slaveName, nullptr, nullptr) != 0) {
        throw std::runtime_error(std::string("openpty: ") + std::strerror(errno));
    }
    ::close(slave);

    TermiosRtuTransport transport(slaveName, 115200);
    transport.open();
    const auto inbound = request(7, 0x1234);
    require(::write(master, inbound.data(), 2) == 2, "PTY fragment write 1 failed");
    auto frames = transport.readFrames(std::chrono::milliseconds(200));
    require(frames.empty(), "transport emitted incomplete frame");
    require(::write(master, inbound.data() + 2, inbound.size() - 2) == static_cast<ssize_t>(inbound.size() - 2),
        "PTY fragment write 2 failed");
    frames = transport.readFrames(std::chrono::milliseconds(200));
    require(frames.size() == 1 && frames.front() == inbound, "termios PTY receive failed");

    const auto outbound = request(9, 0xabcd);
    transport.writeFrame(outbound, std::chrono::milliseconds(500));
    ByteBuffer observed(outbound.size());
    std::size_t total = 0;
    while (total < observed.size()) {
        const auto count = ::read(master, observed.data() + total, observed.size() - total);
        if (count > 0) total += static_cast<std::size_t>(count);
    }
    require(observed == outbound, "termios PTY transmit failed");
    require(transport.parserMetrics().framesAccepted == 1, "transport parser metrics mismatch");
    transport.close();
    ::close(master);
}
}  // namespace

int main() {
    try {
        parserScenarios();
        termiosScenario();
        std::cout << "{\"status\":\"PASS\",\"checks\":" << checks
                  << ",\"scenarios\":[\"fragmented\",\"sticky\",\"noise\","
                     "\"bad_crc_resync\",\"termios_pty_rx_tx\"]}\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "{\"status\":\"FAIL\",\"checks\":" << checks
                  << ",\"error\":\"" << error.what() << "\"}\n";
        return 1;
    }
}

#include "gateway_core.hpp"
#include "mc_driver.hpp"
#include "southbound_reactor.hpp"
#include "command_router.hpp"
#include "probe_timing.hpp"
#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <thread>

using namespace mqmgateway;
using Bytes = std::vector<std::uint8_t>;
using probe::timing;
namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
struct Socket {
    int fd{-1};
    ~Socket() { reset(); }
    void reset(int value = -1) { if (fd >= 0) close(fd); fd = value; }
};
struct Server {
    Socket listener, peer;
    unsigned port{};
    Server() {
        listener.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        check(listener.fd >= 0, "socket");
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener.fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0, "bind");
        socklen_t size = sizeof(address);
        check(getsockname(listener.fd, reinterpret_cast<sockaddr*>(&address), &size) == 0, "getsockname");
        port = ntohs(address.sin_port);
        check(listen(listener.fd, 4) == 0, "listen");
    }
    void ready(int fd) {
        pollfd p{fd, POLLIN, 0};
        check(poll(&p, 1, static_cast<int>(timing::seconds(5).count())) > 0, "socket deadline");
    }
    void acceptPeer() {
        ready(listener.fd);
        peer.reset(accept(listener.fd, nullptr, nullptr));
        check(peer.fd >= 0, "accept");
    }
    Bytes read(std::size_t length) {
        Bytes bytes(length);
        std::size_t at = 0;
        while (at < length) {
            ready(peer.fd);
            const auto n = recv(peer.fd, bytes.data() + at, length - at, 0);
            check(n > 0, "read request");
            at += static_cast<std::size_t>(n);
        }
        return bytes;
    }
    Bytes request() {
        auto bytes = read(9);
        check(Bytes(bytes.begin(), bytes.begin() + 7) == Bytes({0x50,0,0,0xFF,0xFF,3,0}), "MC request header");
        const unsigned length = bytes[7] | (unsigned(bytes[8]) << 8);
        check(length == 12 || length == 14, "MC request length");
        auto rest = read(length);
        bytes.insert(bytes.end(), rest.begin(), rest.end());
        if (!(bytes[9] == 0x10 && bytes[10] == 0 && bytes[11] == 1 &&
              (bytes[12] == 0x04 || bytes[12] == 0x14) && bytes[13] == 0 &&
              bytes[14] == 0 && bytes[18] == 0xA8 && bytes[19] == 1 && bytes[20] == 0))
            throw std::runtime_error("MC 3E binary D-word encoding: " + iot::payloadToHex(bytes));
        return bytes;
    }
    void send(const Bytes& bytes, bool fragmented = false) {
        const auto first = fragmented ? std::size_t{4} : bytes.size();
        check(::send(peer.fd, bytes.data(), first, MSG_NOSIGNAL) == static_cast<ssize_t>(first), "send first");
        if (fragmented)
            check(::send(peer.fd, bytes.data() + first, bytes.size() - first, MSG_NOSIGNAL) ==
                  static_cast<ssize_t>(bytes.size() - first), "send tail");
    }
    void replyRead(unsigned value, bool fragmented = false) {
        send({0xD0,0,0,0xFF,0xFF,3,0,4,0,0,0,
              static_cast<std::uint8_t>(value), static_cast<std::uint8_t>(value >> 8)}, fragmented);
    }
    void replyWrite(unsigned endCode = 0) {
        send({0xD0,0,0,0xFF,0xFF,3,0,2,0,
              static_cast<std::uint8_t>(endCode), static_cast<std::uint8_t>(endCode >> 8)});
    }
};
struct Fixture {
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<edge::UnifiedMessageV2> messages;
    edge::GatewayCore core;
    std::unique_ptr<iot::SouthboundReactor> reactor;
    std::atomic<bool> running{true};
    std::thread thread;
    std::exception_ptr error;
    Fixture() : core([this](auto m) {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(std::move(m)); changed.notify_all();
    }) {}
    ~Fixture() { running = false; if (reactor) reactor->wake(); if (thread.joinable()) thread.join(); core.stop(); }
    void add(const std::string& device, unsigned address, const std::string& point, unsigned port) {
        drivers::McConfig config;
        config.deviceId = device; config.pointId = point;
        config.address = "127.0.0.1"; config.port = port; config.registerAddress = address;
        config.pollMs = static_cast<unsigned>(timing::milliseconds(60).count());
        config.responseMs = static_cast<unsigned>(timing::milliseconds(300).count());
        config.reconnectMs = static_cast<unsigned>(timing::milliseconds(40).count());
        auto driver = std::make_shared<drivers::McDriver>(config, 8);
        check(core.addDriver(driver) && core.addDevice({device, driver->id()}) &&
              core.addPoint({device, point, edge::PointValueType::integer, "", true, address}), "MC register");
    }
    void start() {
        check(core.start(), "core start");
        reactor = std::make_unique<iot::SouthboundReactor>(core.eventDrivers());
        thread = std::thread([this] { try { reactor->run(running); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); error = std::current_exception(); changed.notify_all(); } });
    }
    edge::UnifiedMessageV2 await(const std::string& device, iot::DataType type,
                                  const std::string& status = "", unsigned minimum = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, timing::seconds(5), [&] {
            if (error) return true;
            unsigned count = 0;
            for (const auto& m : messages)
                if (m.deviceId == device && m.dataType == type && m.status == status) ++count;
            return count >= minimum;
        }), "MC message deadline");
        if (error) std::rethrow_exception(error);
        unsigned count = 0;
        for (const auto& m : messages)
            if (m.deviceId == device && m.dataType == type && m.status == status && ++count == minimum) return m;
        throw std::runtime_error("MC message missing");
    }
    void write(const std::string& device, unsigned address, unsigned value) {
        edge::UnifiedMessageV2 m;
        m.deviceId = device; m.dataType = iot::DataType::command;
        m.legacyAddress = address;
        m.rawPayload = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        m.timeout = timing::seconds(3);
        check(core.submit(m), "MC command submit");
    }
};
Bytes awaitWrite(Server& server, unsigned address, unsigned readValue) {
    for (unsigned n = 0; n < 40; ++n) {
        auto request = server.request();
        check((request[15] | (unsigned(request[16]) << 8) | (unsigned(request[17]) << 16)) == address,
              "MC configured address");
        if (request[11] == 0x01 && request[12] == 0x14) return request;
        check(request[11] == 0x01 && request[12] == 0x04, "MC command code");
        server.replyRead(readValue);
    }
    throw std::runtime_error("MC write did not dispatch");
}
}

int main() {
    try {
        Server a, b;
        Fixture f;
        f.add("mc-a", 7, "speed", a.port);
        f.add("mc-b", 8, "temperature", b.port);
        check(f.core.defaultCommand("mc-a") == "mc_write", "MC routing");
        check(!f.core.addDevice({"mc-a", "mc:mc-b"}), "duplicate device rejection");
        iot::CommandRouter router;
        auto routed = router.route("device/mc-a/cmd/mc_write", "{\"register\":7,\"value\":4660}");
        check(routed.accepted && routed.message.payload == Bytes({0x12,0x34}), "MC MQTT command decoding");
        f.start(); a.acceptPeer(); b.acceptPeer();
        check(a.request()[11] == 1, "first read a"); a.replyRead(0x1234, true);
        check(b.request()[11] == 1, "first read b"); b.replyRead(0x0042);
        auto ma = f.await("mc-a", iot::DataType::telemetry);
        auto mb = f.await("mc-b", iot::DataType::telemetry);
        check(ma.pointId == "speed" && ma.cookedValue == "4660" && ma.rawValue == "1234", "MC A point mapping");
        check(mb.pointId == "temperature" && mb.cookedValue == "66", "MC B point mapping");
        f.write("mc-a", 7, 0x1234);
        auto write = awaitWrite(a, 7, 0x1234);
        check(write.size() == 23 && write[7] == 14 && write[21] == 0x34 && write[22] == 0x12,
              "MC write data and length");
        a.replyWrite();
        check(f.await("mc-a", iot::DataType::status, "success").pointId == "speed", "MC acknowledged status");
        f.write("mc-a", 7, 1);
        (void)awaitWrite(a, 7, 0x1234);
        a.replyWrite(0xC051);
        check(f.await("mc-a", iot::DataType::status, "failed").detail.find("49233") != std::string::npos,
              "MC protocol error status");
        // A malformed response must be rejected and followed by a new connection.
        check(a.request()[11] == 1, "post-error read");
        a.send({0xD0,0,0,0xFF,0xFF,3,0,1,0,0});
        a.peer.reset(); a.acceptPeer();
        check(a.request()[11] == 1, "reconnected read"); a.replyRead(3);
        auto second = f.await("mc-a", iot::DataType::telemetry, "", 2);
        check(second.cookedValue == "3", "MC reconnect mapping");
        // Withhold a read reply; the monotonic deadline must reconnect.
        check(a.request()[11] == 1, "timeout read");
        a.acceptPeer();
        check(a.request()[11] == 1, "read after timeout"); a.replyRead(4);
        check(f.await("mc-a", iot::DataType::telemetry, "", 3).cookedValue == "4", "timeout recovery");
        f.write("mc-a", 7, 2);
        (void)awaitWrite(a, 7, 4);
        a.peer.reset(); // Unacknowledged write is never replayed.
        f.await("mc-a", iot::DataType::status, "unknown-result");
        a.acceptPeer();
        auto afterDisconnect = a.request();
        check(afterDisconnect[11] == 1 && afterDisconnect[12] == 0x04, "no write replay");
        f.running = false; f.reactor->wake(); f.thread.join();
        std::cout << "{\"result\":\"PASS\",\"scope\":\"MC 3E binary D-word, two devices, GatewayCore\"";
        f.core.appendMetrics(std::cout); std::cout << "}\n";
    } catch (const std::exception& error) {
        std::cerr << "MC runtime FAIL: " << error.what() << '\n'; return 1;
    }
}

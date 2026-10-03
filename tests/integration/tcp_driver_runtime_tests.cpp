#include "gateway_core.hpp"
#include "modbus_tcp_driver.hpp"
#include "generic_tcp_driver.hpp"
#include "southbound_reactor.hpp"
#include "probe_timing.hpp"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <poll.h>
#include <unistd.h>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <mutex>
#include <thread>
#include <stdexcept>
using namespace mqmgateway;
using probe::timing;
using Bytes = std::vector<uint8_t>;
namespace {
void check(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Socket {
    int fd{-1};
    ~Socket() { if (fd >= 0) close(fd); }
    void reset(int value = -1) { if (fd >= 0) close(fd); fd = value; }
};
struct Server {
    Socket listener, peer;
    unsigned port;
    Server() {
        listener.fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        check(bind(listener.fd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == 0, "bind");
        socklen_t size = sizeof(addr);
        check(getsockname(listener.fd, reinterpret_cast<sockaddr*>(&addr), &size) == 0, "port");
        port = ntohs(addr.sin_port);
        check(listen(listener.fd, 4) == 0, "listen");
    }
    void wait(int fd, short events) {
        pollfd p{fd, events, 0};
        check(poll(&p, 1, static_cast<int>(timing::seconds(5).count())) > 0, "socket deadline");
    }
    void acceptPeer() { wait(listener.fd, POLLIN); peer.reset(accept(listener.fd, nullptr, nullptr)); check(peer.fd >= 0, "accept"); }
    Bytes read(std::size_t count) {
        Bytes b(count); std::size_t offset = 0;
        while (offset < count) {
            wait(peer.fd, POLLIN);
            const auto n = recv(peer.fd, b.data() + offset, count - offset, 0);
            check(n > 0, "read"); offset += n;
        }
        return b;
    }
    void send(const Bytes& b) {
        std::size_t offset = 0;
        while (offset < b.size()) {
            const auto n = ::send(peer.fd, b.data() + offset, b.size() - offset, MSG_NOSIGNAL);
            check(n > 0, "send"); offset += n;
        }
    }
    void reply(const Bytes& request) {
        Bytes b(request.begin(), request.begin() + 7);
        b[5] = 5; b.insert(b.end(), {3, 2, 0, 42});
        send(Bytes(b.begin(), b.begin() + 4));
        send(Bytes(b.begin() + 4, b.end())); // Fragmented MBAP.
    }
};
struct Fixture {
    std::mutex mutex; std::condition_variable changed;
    std::vector<edge::UnifiedMessageV2> messages;
    edge::GatewayCore core;
    std::unique_ptr<iot::SouthboundReactor> reactor;
    std::atomic<bool> running{true};
    std::thread thread;
    std::exception_ptr error;
    Fixture() : core([this](auto m) { std::lock_guard<std::mutex> lock(mutex); messages.push_back(std::move(m)); changed.notify_all(); }) {}
    ~Fixture() { running = false; if (reactor) reactor->wake(); if (thread.joinable()) thread.join(); core.stop(); }
    void start() {
        check(core.start(), "core start");
        reactor = std::make_unique<iot::SouthboundReactor>(core.eventDrivers());
        thread = std::thread([this] {
            try { reactor->run(running); }
            catch (...) { std::lock_guard<std::mutex> lock(mutex); error = std::current_exception(); changed.notify_all(); }
        });
    }
    void await(const std::string& device, iot::DataType type, const std::string& status = "", std::size_t count = 1) {
        std::unique_lock<std::mutex> lock(mutex);
        check(changed.wait_for(lock, timing::seconds(5), [&] {
            if (error) return true;
            std::size_t matches = 0;
            for (const auto& m : messages) if (m.deviceId == device && m.dataType == type && m.status == status) ++matches;
            return matches >= count;
        }), "core message deadline");
        if (error) std::rethrow_exception(error);
    }
    void command(const std::string& device, Bytes payload) {
        edge::UnifiedMessageV2 m;
        m.deviceId = device; m.dataType = iot::DataType::command; m.rawPayload = std::move(payload);
        m.legacyAddress = 7; m.legacySlave = 1; m.timeout = timing::seconds(3);
        check(core.submit(m), "core submit");
    }
};
}
int main() {
    try {
        Server modbus, generic;
        Fixture f;
        drivers::ModbusTcpConfig config;
        config.deviceId = "plc"; config.address = "127.0.0.1"; config.port = modbus.port;
        config.pollMs = timing::milliseconds(50).count();
        config.responseMs = timing::milliseconds(300).count();
        config.reconnectMs = timing::milliseconds(50).count();
        auto plc = std::make_shared<drivers::ModbusTcpDriver>(config, 8);
        check(f.core.addDriver(plc) && f.core.addDevice({"plc", plc->id()}), "register PLC");
        config.deviceId = "sensor"; config.port = generic.port;
        drivers::TcpConfig genericConfig = config;
        auto sensor = std::make_shared<drivers::GenericTcpDriver>(genericConfig, 8);
        check(f.core.addDriver(sensor) && f.core.addDevice({"sensor", sensor->id()}), "register sensor");
        f.start();
        modbus.acceptPeer(); generic.acceptPeer();
        modbus.reply(modbus.read(12));
        f.await("plc", iot::DataType::telemetry);
        generic.send({0}); generic.send({2, 1, 2, 0, 1, 3}); // Fragment + coalesced frames.
        f.await("sensor", iot::DataType::telemetry, "", 2);
        f.command("sensor", {4, 5});
        check(generic.read(4) == Bytes({0, 2, 4, 5}), "generic frame encoding");
        f.await("sensor", iot::DataType::status, "ok");
        f.command("plc", {4, 210});
        Bytes request;
        for (unsigned i = 0; i < 20; ++i) {
            request = modbus.read(12);
            if (request[7] == 6) break;
            modbus.reply(request);
        }
        check(request[7] == 6 && request[9] == 7 && request[10] == 4 && request[11] == 210, "Modbus write encoding");
        modbus.send(request);
        f.await("plc", iot::DataType::status, "ok");
        request = modbus.read(12);
        request[0] ^= 1; modbus.send(request); // Wrong transaction must reset connection.
        modbus.peer.reset(); modbus.acceptPeer();
        modbus.reply(modbus.read(12));
        generic.send({0, 0}); // Invalid frame length -> reconnect.
        generic.peer.reset(); generic.acceptPeer();
        generic.send({0, 1, 9});
        f.await("sensor", iot::DataType::telemetry, "", 3);
        // Deliberately withhold read response -> timeout and reconnect.
        (void)modbus.read(12);
        modbus.acceptPeer();
        modbus.reply(modbus.read(12));
        f.await("plc", iot::DataType::telemetry, "", 3);
        f.command("plc", {4, 211});
        for (unsigned i = 0; i < 20; ++i) {
            request = modbus.read(12);
            if (request[7] == 6) break;
            modbus.reply(request);
        }
        check(request[7] == 6, "second Modbus write dispatch");
        modbus.peer.reset(); // No write echo: outcome unknown, no automatic replay.
        f.await("plc", iot::DataType::status, "unavailable");
        modbus.acceptPeer();
        check(modbus.read(12)[7] == 3, "unconfirmed write not replayed");
        f.running = false; f.reactor->wake(); f.thread.join();
        std::cout << "{\"result\":\"PASS\",\"scope\":\"two TCP devices -> Reactor -> GatewayCore\","
                     "\"checks\":[\"fragmentation\",\"coalescing\",\"read\",\"write\",\"transaction mismatch\",\"invalid length\",\"timeout\",\"reconnect\",\"no_write_replay\"]";
        f.core.appendMetrics(std::cout);
        std::cout << "}\n";
    } catch (const std::exception& e) { std::cerr << "TCP probe FAIL: " << e.what() << '\n'; return 1; }
}

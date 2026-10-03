#include "gateway_core.hpp"
#include "modbus_rtu_driver.hpp"
#include "southbound_reactor.hpp"
#include <rapidjson/document.h>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <exception>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <unistd.h>

using namespace mqmgateway;
using Clock = std::chrono::steady_clock;
namespace {
void require(bool ok, const char* reason) { if (!ok) throw std::runtime_error(reason); }
struct Fd {
    int value{-1};
    ~Fd() { if (value >= 0) ::close(value); }
};
struct Fixture {
    Fd master;
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<edge::UnifiedMessageV2> messages;
    std::exception_ptr reactorError;
    edge::GatewayCore core;
    std::unique_ptr<iot::SouthboundReactor> reactor;
    std::atomic<bool> running{true};
    std::thread receiver;
    Fixture() : core([this](edge::UnifiedMessageV2 message) {
        std::lock_guard<std::mutex> lock(mutex);
        messages.push_back(std::move(message));
        changed.notify_all();
    }) {
        master.value = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
        require(master.value >= 0 && grantpt(master.value) == 0 && unlockpt(master.value) == 0, "PTY creation");
        drivers::RtuConfig config;
        config.device = ptsname(master.value);
        config.pollMs = 30;
        config.responseMs = 150;
        require(core.addDriver(std::make_shared<drivers::ModbusRtuDriver>(config, 8)), "register RTU");
        require(core.start(), "start core");
        reactor = std::make_unique<iot::SouthboundReactor>(core.eventDrivers());
        receiver = std::thread([this] {
            try { reactor->run(running); }
            catch (...) {
                std::lock_guard<std::mutex> lock(mutex);
                reactorError = std::current_exception();
                changed.notify_all();
            }
        });
    }
    ~Fixture() {
        running = false;
        if (reactor) reactor->wake();
        if (receiver.joinable()) receiver.join();
        core.stop();
    }
    serial::ByteBuffer request() {
        serial::ByteBuffer bytes;
        const auto deadline = Clock::now() + std::chrono::seconds(5);
        while (bytes.size() < 8 && Clock::now() < deadline) {
            pollfd fd{master.value, POLLIN, 0};
            const auto remaining = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
            const int ready = poll(&fd, 1, remaining > 0 ? static_cast<int>(remaining) : 0);
            if (ready < 0 && errno == EINTR) continue;
            require(ready > 0, "request deadline");
            std::uint8_t buffer[8];
            const auto n = read(master.value, buffer, 8 - bytes.size());
            if (n < 0 && (errno == EINTR || errno == EAGAIN)) continue;
            require(n > 0, "PTY request read");
            bytes.insert(bytes.end(), buffer, buffer + n);
        }
        require(bytes.size() == 8, "request size");
        const auto crc = serial::RtuFrameParser::crc16(bytes.data(), 6);
        require(bytes[6] == (crc & 255) && bytes[7] == (crc >> 8), "request CRC");
        return bytes;
    }
    void send(const serial::ByteBuffer& bytes) {
        require(write(master.value, bytes.data(), bytes.size()) == static_cast<ssize_t>(bytes.size()), "PTY response write");
    }
    template <typename Predicate> void await(Predicate predicate) {
        std::unique_lock<std::mutex> lock(mutex);
        require(changed.wait_for(lock, std::chrono::seconds(5), [&] { return reactorError || predicate(messages); }), "core message deadline");
        if (reactorError) std::rethrow_exception(reactorError);
    }
    void command(unsigned value) {
        iot::UnifiedMessage message;
        message.deviceId = "rtu-1";
        message.protocol = iot::Protocol::modbus_rtu;
        message.dataType = iot::DataType::command;
        message.direction = iot::Direction::southbound;
        message.address = 7;
        message.payload = {static_cast<std::uint8_t>(value >> 8), static_cast<std::uint8_t>(value)};
        auto command = edge::fromLegacy(message);
        command.driverId = "modbus_rtu";
        require(core.submit(command), "core command dispatch");
    }
    serial::ByteBuffer nextWrite(const serial::ByteBuffer& response) {
        for (unsigned attempt = 0; attempt < 20; ++attempt) {
            auto bytes = request();
            if (bytes[1] == 6) return bytes;
            require(bytes[1] == 3, "unexpected function");
            send(response);
        }
        throw std::runtime_error("write starvation");
    }
};
}
int main() {
    try {
        Fixture f;
        const auto response = serial::RtuFrameParser::appendCrc({1, 3, 2, 0, 42});
        require(f.request()[1] == 3, "scheduled read");
        auto bad = response;
        bad.back() ^= 0xff;
        f.send(bad);
        f.send(response);
        f.await([](const auto& messages) { return !messages.empty(); });
        f.command(1234);
        const auto write = f.nextWrite(response);
        require(write[2] == 0 && write[3] == 7 && write[4] == 4 && write[5] == 210, "register write encoding");
        f.send(write);
        f.await([](const auto& messages) {
            for (const auto& m : messages) if (m.dataType == iot::DataType::status && m.status == "ok") return true;
            return false;
        });
        f.command(1235);
        (void)f.nextWrite(response);  // Deliberate missing slave response.
        f.await([](const auto& messages) {
            for (const auto& m : messages) if (m.dataType == iot::DataType::status && m.status == "timeout") return true;
            return false;
        });
        require(f.request()[1] == 3, "polling recovery");
        f.send(response);
        f.await([](const auto& messages) {
            bool timeout = false;
            for (const auto& m : messages) {
                if (m.status == "timeout") timeout = true;
                else if (timeout && m.dataType == iot::DataType::telemetry) return true;
            }
            return false;
        });
        f.running = false;
        f.reactor->wake();
        f.receiver.join();
        std::ostringstream out;
        out << "{\"result\":\"PASS\",\"scope\":\"real PTY -> RTU driver -> shared reactor -> GatewayCore\"";
        f.core.appendMetrics(out);
        out << "}";
        rapidjson::Document metrics;
        metrics.Parse(out.str().c_str());
        require(!metrics.HasParseError(), "metrics JSON");
        require(metrics["gateway_core"]["commands_submitted"].GetUint64() == 2, "core commands counter");
        require(metrics["gateway_core"]["dropped_messages"].GetUint64() == 0, "core drops");
        require(metrics["rtu"]["writes_confirmed"].GetUint64() == 1, "RTU write confirmation");
        require(metrics["rtu"]["timeouts"].GetUint64() >= 1, "RTU timeout counter");
        require(metrics["rtu"]["crc_candidates_rejected"].GetUint64() > 0, "RTU CRC rejection counter");
        std::cout << out.str() << '\n';
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "RTU driver runtime FAIL: " << error.what() << '\n';
        return 1;
    }
}

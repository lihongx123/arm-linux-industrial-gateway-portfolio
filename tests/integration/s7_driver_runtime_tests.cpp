#include "gateway_core.hpp"
#include "s7_driver.hpp"
#include "acquisition_scheduler.hpp"
#include "command_router.hpp"
#include "probe_timing.hpp"
#include <snap7.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <iostream>
#include <mutex>
#include <stdexcept>
#include <sstream>
#include <thread>
#include <vector>

using namespace mqmgateway;
using probe::timing;

namespace {
void check(bool value, const char* why) { if (!value) throw std::runtime_error(why); }
std::uint16_t freePort() {
    const int fd = socket(AF_INET, SOCK_STREAM, 0);
    check(fd >= 0, "S7 test port socket");
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    const int bound = bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address));
    socklen_t length = sizeof(address);
    const int named = bound == 0 ? getsockname(fd, reinterpret_cast<sockaddr*>(&address), &length) : -1;
    close(fd);
    check(bound == 0 && named == 0, "S7 test port allocation");
    return ntohs(address.sin_port);
}

struct Snap7Server {
    TS7Server server;
    std::uint16_t port{freePort()};
    std::uint8_t db1[2]{0, 42};
    std::uint8_t db2[2]{0, 84};
    bool started{false};

    Snap7Server() {
        check(server.SetParam(p_u16_LocalPort, &port) == 0, "S7 server port config");
        check(server.RegisterArea(srvAreaDB, 1, db1, sizeof(db1)) == 0, "register DB1");
        check(server.RegisterArea(srvAreaDB, 2, db2, sizeof(db2)) == 0, "register DB2");
        const int rc = server.StartTo("127.0.0.1");
        if (rc != 0) {
            throw std::runtime_error("Snap7 server StartTo(127.0.0.1) failed: " + std::to_string(rc));
        }
        started = true;
    }
    ~Snap7Server() { if (started) server.Stop(); }
};
}

int main() {
    try {
        Snap7Server plc;
        std::mutex mutex;
        std::condition_variable changed;
        std::vector<edge::UnifiedMessageV2> messages;
        edge::GatewayCore core([&](auto message) {
            std::lock_guard<std::mutex> lock(mutex);
            messages.push_back(std::move(message));
            changed.notify_all();
        });

        drivers::S7Config a;
        a.deviceId = "s7-a"; a.pointId = "word_a"; a.address = "127.0.0.1";
        a.port = plc.port; a.dbNumber = 1; a.byteOffset = 0; a.intervalMs = 50; a.writable = true;
        drivers::S7Config b = a;
        b.deviceId = "s7-b"; b.pointId = "word_b"; b.dbNumber = 2;
        auto da = std::make_shared<drivers::S7Driver>(a);
        auto db = std::make_shared<drivers::S7Driver>(b);
        drivers::S7Config bad = a;
        bad.deviceId = "s7-bad"; bad.pointId = "missing"; bad.dbNumber = 99;
        auto badDriver = std::make_shared<drivers::S7Driver>(bad);
        check(core.addDriver(da) && core.addDriver(db) && core.addDriver(badDriver), "S7 driver registration");
        check(core.addDevice({a.deviceId, da->id()}) && core.addDevice({b.deviceId, db->id()}), "S7 device registry");
        check(core.addDevice({bad.deviceId, badDriver->id()}), "S7 bad-address device registry");
        check(!core.addDevice({a.deviceId, db->id()}), "S7 duplicate device rejection");
        check(core.addPoint({a.deviceId, a.pointId, edge::PointValueType::integer, "DB1.DBW0", true, 0}) &&
              core.addPoint({b.deviceId, b.pointId, edge::PointValueType::integer, "DB2.DBW0", true, 0}) &&
              core.addPoint({bad.deviceId, bad.pointId, edge::PointValueType::integer, "DB99.DBW0", false, 0}),
              "S7 point registry");
        check(core.defaultCommand(a.deviceId) == "s7_write", "S7 command route");
        iot::CommandRouter router;
        auto routed = router.route("device/s7-a/cmd/s7_write", "{\"value\":4660}");
        check(routed.accepted && routed.message.payload == std::vector<std::uint8_t>({0x12, 0x34}),
              "S7 MQTT command decoding");

        check(core.start(), "S7 core start");
        edge::AcquisitionScheduler scheduler(core.acquisitionDrivers());
        scheduler.start();
        auto awaitTelemetry = [&](const std::string& device, const std::string& value, unsigned count = 1) {
            std::unique_lock<std::mutex> lock(mutex);
            check(changed.wait_for(lock, timing::seconds(8), [&] {
                unsigned seen = 0;
                for (const auto& message : messages)
                    if (message.deviceId == device && message.dataType == iot::DataType::telemetry &&
                        message.cookedValue == value && ++seen >= count) return true;
                return false;
            }), "S7 telemetry deadline");
        };
        awaitTelemetry(a.deviceId, "42");
        awaitTelemetry(b.deviceId, "84");
        {
            std::lock_guard<std::mutex> lock(mutex);
            bool seenA = false, seenB = false;
            for (const auto& message : messages) {
                if (message.deviceId == a.deviceId && message.sourceDescriptor == "DB1.DBW0") seenA = true;
                if (message.deviceId == b.deviceId && message.sourceDescriptor == "DB2.DBW0") seenB = true;
            }
            check(seenA && seenB, "S7 DB address metadata");
        }

        edge::UnifiedMessageV2 command;
        command.deviceId = a.deviceId; command.pointId = a.pointId;
        command.dataType = iot::DataType::command; command.rawPayload = {0x00, 0x77};
        check(core.submit(command), "S7 write submit");
        {
            std::unique_lock<std::mutex> lock(mutex);
            check(changed.wait_for(lock, timing::seconds(5), [&] {
                for (const auto& message : messages)
                    if (message.deviceId == a.deviceId && message.dataType == iot::DataType::status &&
                        message.status == "success") return true;
                return false;
            }), "S7 write status deadline");
        }
        check(plc.db1[0] == 0 && plc.db1[1] == 0x77, "S7 DB write bytes");
        awaitTelemetry(a.deviceId, "119", 2);

        auto metrics = [&](const std::shared_ptr<drivers::S7Driver>& driver) {
            std::ostringstream out; driver->appendMetrics(out); return out.str();
        };
        auto until = [&](const std::function<bool()>& done, const char* why) {
            const auto deadline = std::chrono::steady_clock::now() + timing::seconds(12);
            while (!done() && std::chrono::steady_clock::now() < deadline)
                std::this_thread::sleep_for(timing::milliseconds(30));
            check(done(), why);
        };
        until([&] { return metrics(badDriver).find("\"errors\":0") == std::string::npos; },
              "S7 missing DB must report protocol error");
        until([&] { return metrics(badDriver).find("\"connected\":false") != std::string::npos; },
              "S7 bad address disconnect state");

        check(plc.server.Stop() == 0, "S7 server stop");
        plc.started = false;
        until([&] { return metrics(da).find("\"connected\":false") != std::string::npos; },
              "S7 client detects disconnect");
        unsigned beforeRestart = 0;
        {
            std::lock_guard<std::mutex> lock(mutex);
            for (const auto& message : messages)
                if (message.deviceId == a.deviceId && message.dataType == iot::DataType::telemetry &&
                    message.cookedValue == "119") ++beforeRestart;
        }
        check(plc.server.StartTo("127.0.0.1") == 0, "S7 server restart");
        plc.started = true;
        awaitTelemetry(a.deviceId, "119", beforeRestart + 1);
        until([&] { return metrics(da).find("\"connected\":true") != std::string::npos; },
              "S7 client reconnects");

        scheduler.stop();
        core.stop();
        std::cout << "{\"result\":\"PASS\",\"scope\":\"real local Snap7 server -> S7 client -> GatewayCore\",\"checks\":[\"connect\",\"read\",\"write\",\"two_devices\",\"bad_db\",\"disconnect\",\"reconnect\",\"point_mapping\",\"db_metadata\",\"command_route\"]}\n";
    } catch (const std::exception& error) {
        std::cerr << "S7 runtime FAIL: " << error.what() << '\n';
        return 1;
    }
}

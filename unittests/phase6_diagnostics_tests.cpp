#include "alarm_event.hpp"
#include "diagnostics.hpp"
#include "gateway_core.hpp"
#include "watchdog.hpp"
#include <catch2/catch_test_macros.hpp>
#include <rapidjson/document.h>
#include <memory>
#include <sstream>

using namespace mqmgateway;

TEST_CASE("Device health uses monotonic freshness and de-duplicates alarm edges", "[phase6]") {
    using Clock = edge::TimeService::Monotonic;
    auto now = Clock::time_point{};
    auto wall = edge::TimeService::Wall::time_point(std::chrono::seconds(1000));
    edge::TimeService time([&] { return now; }, [&] { return wall; });
    edge::DiagnosticsManager diagnostics({2, 2, std::chrono::milliseconds(100)}, time);
    REQUIRE(diagnostics.registerDevice("device-1"));
    REQUIRE_FALSE(diagnostics.registerDevice("device-1"));
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::unknown);
    now += std::chrono::milliseconds(99);
    diagnostics.evaluate();
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::unknown);
    now += std::chrono::milliseconds(1);
    diagnostics.evaluate();
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::offline);
    diagnostics.evaluate(); // No repeated offline alarm.

    edge::UnifiedMessageV2 good;
    good.deviceId = "device-1";
    good.dataType = iot::DataType::telemetry;
    diagnostics.observe(good);
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::offline);
    diagnostics.observe(good);
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::online);
    edge::UnifiedMessageV2 bad = good;
    bad.dataType = iot::DataType::status;
    bad.status = "timeout";
    bad.detail = "read timed out";
    diagnostics.observe(bad);
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::online);
    diagnostics.observe(bad);
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::degraded);
    REQUIRE(diagnostics.acknowledgeAlarm("health/device-1"));
    REQUIRE_FALSE(diagnostics.acknowledgeAlarm("health/device-1"));
    // Moving wall time backwards must not delay the monotonic offline deadline.
    wall -= std::chrono::hours(1);
    now += std::chrono::milliseconds(100);
    diagnostics.evaluate();
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::offline);
    diagnostics.observe(good);
    diagnostics.observe(good);
    REQUIRE(diagnostics.health("device-1")->state == edge::HealthState::online);

    std::ostringstream output;
    diagnostics.appendMetrics(output);
    rapidjson::Document json;
    json.Parse(("{" + output.str().substr(1) + "}").c_str());
    REQUIRE_FALSE(json.HasParseError());
    REQUIRE(json["diagnostics"]["device_health"]["online"].GetUint() == 1);
    REQUIRE(json["diagnostics"]["alarms"]["active"].GetUint() == 0);
    REQUIRE(json["diagnostics"]["alarms"]["events_total"].GetUint64() == 6);
}

TEST_CASE("Alarm history and diagnostic detail output stay bounded and escaped", "[phase6]") {
    edge::AlarmLedger ledger;
    for (int i = 0; i < 150; ++i) {
        ledger.setSystemAlarm("bad\"\nname", true, i * 2);
        ledger.setSystemAlarm("bad\"\nname", true, i * 2); // duplicate edge
        ledger.setSystemAlarm("bad\"\nname", false, i * 2 + 1);
    }
    REQUIRE(ledger.eventCount() == 300);
    REQUIRE(ledger.historySize() == 256);
    REQUIRE(ledger.activeCount() == 0);
    const auto recentEvents = ledger.eventsSince(297);
    REQUIRE(recentEvents.size() == 3);
    REQUIRE(recentEvents.front().sequence == 298);
    REQUIRE(recentEvents.back().sequence == 300);
    std::ostringstream output;
    ledger.appendMetrics(output);
    rapidjson::Document json;
    json.Parse(output.str().c_str());
    REQUIRE_FALSE(json.HasParseError());
    REQUIRE(json["history_stored"].GetUint() == 256);
    REQUIRE(json["recent"].Size() == 8);
    REQUIRE(std::string(json["recent"][0]["key"].GetString()) == "system/bad\"\nname");
}

TEST_CASE("Queue watchdog reports only a genuine backed-up stall and recovery", "[phase6]") {
    using Clock = edge::TimeService::Monotonic;
    edge::QueueWatchdog watchdog(std::chrono::milliseconds(100));
    const auto start = Clock::time_point{};
    REQUIRE_FALSE(watchdog.observe(0, 0, start));
    REQUIRE_FALSE(watchdog.observe(4, 0, start + std::chrono::milliseconds(20)));
    REQUIRE_FALSE(watchdog.observe(4, 0, start + std::chrono::milliseconds(119)));
    REQUIRE(watchdog.observe(4, 0, start + std::chrono::milliseconds(120)) ==
            edge::QueueWatchdog::Transition::stalled);
    REQUIRE_FALSE(watchdog.observe(4, 0, start + std::chrono::milliseconds(200)));
    REQUIRE(watchdog.observe(3, 1, start + std::chrono::milliseconds(201)) ==
            edge::QueueWatchdog::Transition::recovered);
    REQUIRE_FALSE(watchdog.observe(0, 1, start + std::chrono::seconds(1)));
    std::ostringstream output;
    watchdog.appendMetrics(output);
    rapidjson::Document json;
    json.Parse(("{" + output.str().substr(1) + "}").c_str());
    REQUIRE_FALSE(json.HasParseError());
    REQUIRE(json["queue_watchdog"]["stall_events"].GetUint64() == 1);
    REQUIRE(json["queue_watchdog"]["recoveries"].GetUint64() == 1);
}

TEST_CASE("GatewayCore reports health only after a registered point passes mapping", "[phase6]") {
    class Driver final : public edge::IDeviceDriver {
    public:
        std::string id() const override { return "test-driver"; }
        void setMessageSink(Emit emit) override { emit_ = std::move(emit); }
        void start() override {}
        void stop() noexcept override { emit_ = {}; }
        bool submit(const edge::UnifiedMessageV2&) override { return true; }
        void send(edge::UnifiedMessageV2 message) { if (emit_) emit_(std::move(message)); }
    private:
        Emit emit_;
    };
    std::vector<edge::UnifiedMessageV2> delivered;
    edge::GatewayCore core([&](auto message) { delivered.push_back(std::move(message)); });
    auto driver = std::make_shared<Driver>();
    REQUIRE(core.addDriver(driver));
    REQUIRE(core.addDevice({"device-1", driver->id()}));
    REQUIRE(core.addPoint({"device-1", "temperature", edge::PointValueType::integer}));
    REQUIRE(core.start());
    edge::UnifiedMessageV2 message;
    message.deviceId = "device-1";
    message.pointId = "wrong-point";
    message.rawPayload = {0, 42};
    driver->send(message);
    REQUIRE(delivered.empty());
    REQUIRE(core.deviceHealth("device-1")->state == edge::HealthState::unknown);
    message.pointId = "temperature";
    driver->send(message);
    REQUIRE(delivered.size() == 1);
    REQUIRE(delivered.front().cookedValue == "42");
    REQUIRE(core.deviceHealth("device-1")->state == edge::HealthState::online);
    core.reportQueueStall(true);
    REQUIRE(core.acknowledgeAlarm("system/queue_stalled"));
    core.reportQueueStall(false);
    std::ostringstream metrics;
    core.appendMetrics(metrics);
    rapidjson::Document json;
    json.Parse(("{" + metrics.str().substr(1) + "}").c_str());
    REQUIRE_FALSE(json.HasParseError());
    REQUIRE(json["gateway_core"]["mapping_failures"].GetUint64() == 1);
    REQUIRE(json["diagnostics"]["device_health"]["online"].GetUint() == 1);
    REQUIRE(json["diagnostics"]["alarms"]["active"].GetUint() == 0);
    core.stop();
}

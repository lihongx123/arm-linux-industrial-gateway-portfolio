#include "device_registry.hpp"
#include "driver_manager.hpp"
#include "gateway_core.hpp"
#include "point_mapper.hpp"
#include "point_registry.hpp"
#include "unified_message_v2.hpp"

#include <catch2/catch_test_macros.hpp>

#include <memory>
#include <stdexcept>

using namespace mqmgateway;

namespace {

class FakeDriver final : public edge::IDeviceDriver {
public:
    explicit FakeDriver(std::string driverId) : driverId_(std::move(driverId)) {}

    std::string id() const override { return driverId_; }
    void setMessageSink(Emit emit) override { emit_ = std::move(emit); }
    void start() override { ++starts; }
    void stop() noexcept override { ++stops; }
    bool submit(const edge::UnifiedMessageV2& command) override {
        lastDevice = command.deviceId;
        ++commands;
        return true;
    }
    void emit(edge::UnifiedMessageV2 message) const {
        if (emit_) emit_(std::move(message));
    }

    int starts{0};
    int stops{0};
    int commands{0};
    std::string lastDevice;

private:
    std::string driverId_;
    Emit emit_;
};

}  // namespace

TEST_CASE("Device and point registries reject duplicate or ambiguous identities", "[edge]") {
    edge::DeviceRegistry devices;
    REQUIRE_FALSE(devices.add({"", "can"}));
    REQUIRE_FALSE(devices.add({"d1", ""}));
    REQUIRE(devices.add({"d1", "can"}));
    REQUIRE_FALSE(devices.add({"d1", "rtu"}));
    REQUIRE(devices.find("d1")->driverId == "can");
    REQUIRE_FALSE(devices.find("unknown"));

    edge::PointRegistry points;
    REQUIRE_FALSE(points.add({"", "p", edge::PointValueType::integer}));
    REQUIRE(points.add({"a", "bc", edge::PointValueType::integer, "rpm", false, 12}));
    REQUIRE(points.add({"ab", "c", edge::PointValueType::integer, "rpm", true, 13}));
    REQUIRE_FALSE(points.add({"a", "bc", edge::PointValueType::boolean}));
    REQUIRE(points.find("a", "bc")->address == 12);
    REQUIRE(points.find("ab", "c")->writable);
    REQUIRE(points.remove("a", "bc"));
    REQUIRE_FALSE(points.find("a", "bc"));
    REQUIRE(devices.remove("d1"));
}

TEST_CASE("Driver manager routes by registered device, not protocol branch", "[edge]") {
    edge::DeviceRegistry devices;
    REQUIRE(devices.add({"motor-1", "fake"}));
    edge::PointRegistry points;
    REQUIRE(points.add({"motor-1", "speed", edge::PointValueType::integer, "rpm", true, 1}));
    REQUIRE(points.add({"motor-1", "temperature", edge::PointValueType::integer, "C", false, 2}));
    edge::DriverManager manager;
    auto driver = std::make_shared<FakeDriver>("fake");
    REQUIRE(manager.add(driver));
    REQUIRE_FALSE(manager.add(std::make_shared<FakeDriver>("fake")));
    edge::UnifiedMessageV2 command;
    command.deviceId = "motor-1";
    command.dataType = iot::DataType::command;
    command.pointId = "speed";
    REQUIRE_FALSE(manager.submit(devices, points, command));
    REQUIRE(manager.startAll());
    REQUIRE_FALSE(manager.startAll());
    REQUIRE_FALSE(manager.add(std::make_shared<FakeDriver>("new")));
    REQUIRE(manager.submit(devices, points, command));
    REQUIRE(driver->commands == 1);
    REQUIRE(driver->lastDevice == "motor-1");
    command.pointId = "temperature";
    REQUIRE_FALSE(manager.submit(devices, points, command));
    command.pointId = "missing";
    REQUIRE_FALSE(manager.submit(devices, points, command));
    command.pointId = "speed";
    command.driverId = "wrong-driver";
    REQUIRE_FALSE(manager.submit(devices, points, command));
    command.driverId.clear();
    command.deviceId = "missing";
    REQUIRE_FALSE(manager.submit(devices, points, command));
    manager.stopAll();
    REQUIRE(driver->starts == 1);
    REQUIRE(driver->stops == 1);
    REQUIRE_FALSE(manager.submit(devices, points, command));
}

TEST_CASE("V1 to V2 migration envelope preserves legacy message fields", "[edge]") {
    iot::UnifiedMessage old;
    old.deviceId = "rtu-7";
    old.protocol = iot::Protocol::modbus_rtu;
    old.direction = iot::Direction::southbound;
    old.dataType = iot::DataType::command;
    old.quality = iot::Quality::timeout;
    old.payload = {0x12, 0x34};
    old.address = 42;
    old.slave = 7;
    old.timeout = std::chrono::milliseconds(500);
    const auto v2 = edge::fromLegacy(old);
    REQUIRE(v2.version == 2);
    const auto restored = edge::toLegacy(v2);
    REQUIRE(restored.deviceId == old.deviceId);
    REQUIRE(restored.protocol == old.protocol);
    REQUIRE(restored.direction == old.direction);
    REQUIRE(restored.dataType == old.dataType);
    REQUIRE(restored.quality == old.quality);
    REQUIRE(restored.payload == old.payload);
    REQUIRE(restored.address == old.address);
    REQUIRE(restored.slave == old.slave);
    REQUIRE(restored.timeout == old.timeout);
    REQUIRE(restored.timestamp == old.timestamp);
    REQUIRE(restored.enqueuedAt == old.enqueuedAt);
    auto invalid = v2;
    invalid.version = 3;
    REQUIRE_THROWS_AS(edge::toLegacy(invalid), std::invalid_argument);
}

TEST_CASE("Gateway core validates registered devices and points without protocol branching", "[edge]") {
    std::vector<edge::UnifiedMessageV2> published;
    edge::GatewayCore core([&](edge::UnifiedMessageV2 message) {
        published.push_back(std::move(message));
    });
    auto driver = std::make_shared<FakeDriver>("fieldbus");
    REQUIRE_FALSE(core.addDevice({"unknown", "missing-driver"}));
    REQUIRE(core.addDriver(driver));
    REQUIRE_FALSE(core.addPoint({"motor", "speed", edge::PointValueType::integer}));
    REQUIRE(core.addDevice({"motor", "fieldbus"}));
    REQUIRE(core.addPoint({"motor", "speed", edge::PointValueType::integer, "rpm", true}));
    REQUIRE(core.start());

    edge::UnifiedMessageV2 command;
    command.deviceId = "motor";
    command.pointId = "speed";
    command.dataType = iot::DataType::command;
    REQUIRE(core.submit(command));

    edge::UnifiedMessageV2 telemetry;
    telemetry.deviceId = "motor";
    telemetry.pointId = "speed";
    telemetry.driverId = "fieldbus";
    telemetry.rawPayload = {0, 42};
    driver->emit(telemetry);
    REQUIRE(published.size() == 1);
    REQUIRE(published.front().cookedValue == "42");
    telemetry.pointId = "unknown";
    driver->emit(telemetry);
    REQUIRE(published.size() == 1);
    telemetry.pointId = "speed";
    telemetry.driverId = "wrong";
    driver->emit(telemetry);
    // The manager stamps the registered driver identity rather than trusting a payload.
    REQUIRE(published.size() == 2);
    core.stop();
    telemetry.driverId = "fieldbus";
    driver->emit(telemetry);
    REQUIRE(published.size() == 2);
}

TEST_CASE("Point mapper keeps wire payload and decodes scaled register value", "[edge]") {
    edge::UnifiedMessageV2 message;
    message.deviceId = "rtu-1";
    message.rawPayload = {1, 3, 2, 0, 42, 0, 0};
    message.pointRawPayload = {0, 42};
    edge::PointDefinition point{"rtu-1", "holding-7", edge::PointValueType::integer, "C", false, 7, 0.5, -10};
    REQUIRE(edge::PointMapper::apply(message, point));
    REQUIRE(message.pointId == "holding-7");
    REQUIRE(message.rawValue == "002a");
    REQUIRE(message.cookedValue == "11");
    REQUIRE(message.rawPayload.size() == 7);
    REQUIRE_FALSE(edge::PointMapper::apply(message, {"other", "holding-7"}));
}

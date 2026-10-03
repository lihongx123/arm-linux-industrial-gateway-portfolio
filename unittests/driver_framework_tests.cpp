#include "gateway_core.hpp"
#include "command_router.hpp"
#include "can_driver.hpp"
#include "modbus_rtu_driver.hpp"
#include <catch2/catch_test_macros.hpp>
#include <sstream>
#include <stdexcept>

using namespace mqmgateway;

namespace {
class ProbeDriver final : public edge::IDeviceDriver {
public:
    ProbeDriver(std::string name, std::string device) : name_(std::move(name)), device_(std::move(device)) {}
    std::string id() const override { return name_; }
    void setMessageSink(Emit sink) override { sink_ = std::move(sink); }
    bool acceptsDevice(const std::string& device) const override { return device == device_; }
    std::string defaultCommand() const override { return "can_tx"; }
    void start() override {
        ++starts;
        if (failStart) throw std::runtime_error("partial driver startup");
        edge::UnifiedMessageV2 message;
        message.deviceId = device_;
        if (sink_) sink_(message);  // Reentrant discovery during lifecycle startup.
    }
    void stop() noexcept override { ++stops; }
    bool submit(const edge::UnifiedMessageV2& command) override {
        ++commands;
        auto result = command;
        result.dataType = iot::DataType::status;
        if (sink_) sink_(result);
        return true;
    }
    bool hasSink() const { return bool(sink_); }
    bool failStart{false};
    int starts{0}, stops{0}, commands{0};
private:
    std::string name_, device_;
    Emit sink_;
};
}

TEST_CASE("Driver startup rollback cleans the partially started driver and can retry", "[edge][drivers]") {
    auto driver = std::make_shared<ProbeDriver>("probe", "device-a");
    driver->failStart = true;
    unsigned emissions = 0;
    edge::GatewayCore core([&](edge::UnifiedMessageV2) { ++emissions; });
    REQUIRE(core.addDriver(driver));
    REQUIRE_THROWS_AS(core.start(), std::runtime_error);
    REQUIRE(driver->stops == 1);
    REQUIRE_FALSE(driver->hasSink());
    driver->failStart = false;
    REQUIRE(core.start());
    REQUIRE(emissions == 1);
    edge::UnifiedMessageV2 command;
    command.deviceId = "device-a";
    command.dataType = iot::DataType::command;
    REQUIRE(core.submit(command));
    REQUIRE(emissions == 2);
    REQUIRE(driver->commands == 1);
    core.stop();
    REQUIRE(driver->stops == 2);
    REQUIRE_FALSE(driver->hasSink());
    REQUIRE_FALSE(core.submit(command));
}

TEST_CASE("Ambiguous ownership is rejected and explicit registration resolves it", "[edge][drivers]") {
    edge::GatewayCore core({});
    REQUIRE(core.addDriver(std::make_shared<ProbeDriver>("a", "shared")));
    REQUIRE(core.addDriver(std::make_shared<ProbeDriver>("b", "shared")));
    REQUIRE(core.defaultCommand("shared").empty());
    edge::UnifiedMessageV2 command;
    command.deviceId = "shared";
    command.dataType = iot::DataType::command;
    REQUIRE_FALSE(core.prepare(command));
    REQUIRE(core.addDevice({"shared", "b"}));
    command.driverId = "a";
    REQUIRE_FALSE(core.prepare(command));
    command.driverId = "b";
    REQUIRE(core.prepare(command));
}

TEST_CASE("Cloud command selection uses its resolver and preserves built-in command validation", "[edge][drivers]") {
    iot::CommandRouter router;
    const auto result = router.routeCloud("resume/gateway/devices/cloud-soak-001/command",
        R"({"can_id":291,"data":"01"})", [](const std::string&) { return "can_tx"; });
    REQUIRE(result.accepted);
    REQUIRE(result.driverId == "can");
    const auto rtu = router.routeCloud("resume/gateway/devices/rtu-7/command",
        R"({"slave":7,"register":3,"value":55})", [](const std::string&) { return "modbus_write"; });
    REQUIRE(rtu.accepted);
    REQUIRE(rtu.driverId == "modbus_rtu");
    REQUIRE_FALSE(router.routeCloud("resume/gateway/devices/rtu-7/command", "{}",
        [](const std::string&) { return std::string{}; }).accepted);
    REQUIRE_FALSE(router.routeCloud("resume/gateway/devices/a/b/command", "{}",
        [](const std::string&) { return "can_tx"; }).accepted);
    REQUIRE_FALSE(router.route("device/rtu-7/cmd/modbus_write",
        R"({"slave":8,"register":3,"value":55})").accepted);
}

TEST_CASE("Explicit RTU binding coexists with legacy CAN aliases", "[edge][drivers]") {
    drivers::CanDriver can("unused", false);
    drivers::RtuConfig config;
    config.device = "/not-opened";
    config.slave = 7;
    drivers::ModbusRtuDriver rtu(config, 4);
    REQUIRE(can.acceptsDevice("can-291"));
    REQUIRE(can.acceptsDevice("can0"));
    REQUIRE(can.acceptsDevice("cloud-soak-001"));
    REQUIRE(can.acceptsDevice("rtu-7")); // CAN does not infer ownership from protocol prefixes.
    REQUIRE(rtu.acceptsDevice("rtu-7"));
    REQUIRE_FALSE(rtu.acceptsDevice("rtu-8"));
    REQUIRE_FALSE(rtu.acceptsDevice("can0"));
    config.device.clear();
    drivers::ModbusRtuDriver disabled(config, 4);
    REQUIRE_FALSE(disabled.acceptsDevice("rtu-7"));
    config.device = "/not-opened";
    edge::GatewayCore core([](auto) {});
    REQUIRE(core.addDriver(std::make_shared<drivers::CanDriver>("unused", false)));
    REQUIRE(core.addDriver(std::make_shared<drivers::ModbusRtuDriver>(config, 4)));
    REQUIRE(core.addDevice({"rtu-7", "modbus_rtu"}));
    REQUIRE(core.defaultCommand("rtu-7") == "modbus_write");
    REQUIRE(core.defaultCommand("cloud-soak-001") == "can_tx");
}

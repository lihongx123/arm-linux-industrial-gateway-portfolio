#include "mqtt_northbound_adapter.hpp"
#include "northbound_manager.hpp"
#include "unified_message_v2.hpp"
#include <catch2/catch_test_macros.hpp>
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <sstream>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

using namespace mqmgateway;
namespace {
class MockAdapter final : public northbound::INorthboundAdapter {
public:
    explicit MockAdapter(std::string name) : name_(std::move(name)) {}
    std::string id() const override { return name_; }
    bool start() override { started = true; return true; }
    void stop() noexcept override { started = false; }
    bool healthy() const override { return started && !forceUnhealthy; }
    northbound::PublishResult publish(const edge::UnifiedMessageV2& message) override {
        received.push_back(message);
        if (fail) return {false, "mock transport error"};
        return {true, {}};
    }
    void setCommandHandler(CommandHandler callback) override { handler = std::move(callback); }
    northbound::AdapterMetrics metrics() const override {
        return {static_cast<std::uint64_t>(received.size()), 0, 0, 0, healthy()};
    }
    std::string name_;
    bool started{false}, fail{false}, forceUnhealthy{false};
    CommandHandler handler;
    std::vector<edge::UnifiedMessageV2> received;
};

edge::UnifiedMessageV2 command(const std::string& id = "cmd-1") {
    edge::UnifiedMessageV2 message;
    message.deviceId = "d1";
    message.correlationId = id;
    message.timeout = std::chrono::milliseconds(1000);
    return message;
}
}

TEST_CASE("Northbound semantic categories and legacy correlation survive conversion", "[northbound]") {
    const edge::NorthboundType types[] = {
        edge::NorthboundType::telemetry, edge::NorthboundType::attribute, edge::NorthboundType::status,
        edge::NorthboundType::alarm, edge::NorthboundType::diagnostic, edge::NorthboundType::command,
        edge::NorthboundType::command_result};
    for (unsigned a = 0; a < 7; ++a)
        for (unsigned b = a + 1; b < 7; ++b) REQUIRE(types[a] != types[b]);
    iot::UnifiedMessage legacy;
    legacy.correlationId = "rtu-cmd-42";
    legacy.dataType = iot::DataType::status;
    const auto upgraded = edge::fromLegacy(legacy);
    REQUIRE(upgraded.northboundType == edge::NorthboundType::status);
    REQUIRE(edge::toLegacy(upgraded).correlationId == legacy.correlationId);
}

TEST_CASE("Northbound manager fans out immutable messages and isolates adapter failure", "[northbound]") {
    northbound::NorthboundManager manager;
    auto a = std::make_unique<MockAdapter>("a");
    auto b = std::make_unique<MockAdapter>("b");
    auto* first = a.get();
    auto* second = b.get();
    REQUIRE(manager.add(std::move(a)));
    REQUIRE(manager.add(std::move(b)));
    REQUIRE_FALSE(manager.add(std::make_unique<MockAdapter>("a")));
    REQUIRE(manager.start());
    REQUIRE(manager.healthy());
    first->fail = true;
    edge::UnifiedMessageV2 message;
    message.deviceId = "d1";
    message.cookedValue = "123";
    const auto results = manager.publish(message);
    REQUIRE(results.size() == 2);
    REQUIRE_FALSE(results[0].second.accepted);
    REQUIRE(results[1].second.accepted);
    REQUIRE(first->received.size() == 1);
    REQUIRE(second->received.size() == 1);
    REQUIRE(second->received[0].cookedValue == "123");
    REQUIRE(message.cookedValue == "123");
    second->forceUnhealthy = true;
    REQUIRE_FALSE(manager.healthy());
    second->forceUnhealthy = false;
    manager.stop();
    REQUIRE_FALSE(manager.healthy());
}

TEST_CASE("Northbound command admission rejects duplicate, expired, overloaded and invalid commands", "[northbound]") {
    northbound::NorthboundManager manager(2);
    auto adapter = std::make_unique<MockAdapter>("in");
    auto* mock = adapter.get();
    REQUIRE(manager.add(std::move(adapter)));
    int accepted = 0;
    manager.setCommandHandler([&](edge::UnifiedMessageV2& message) -> std::string {
        ++accepted;
        return message.operation == "reject" ? "driver unavailable" : std::string{};
    });
    REQUIRE(manager.start());
    auto valid = command();
    manager.receiveCommand("in", valid);
    REQUIRE(accepted == 1);
    REQUIRE(mock->received.back().status == "accepted");
    REQUIRE(mock->received.back().correlationId == "cmd-1");
    manager.receiveCommand("in", valid);
    REQUIRE(accepted == 1);
    REQUIRE(mock->received.back().status == "rejected");
    REQUIRE(mock->received.back().detail == "duplicate command_id");
    auto expired = command("expired");
    expired.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    manager.receiveCommand("in", expired);
    REQUIRE(mock->received.back().status == "timeout");
    auto rejected = command("cmd-2");
    rejected.operation = "reject";
    manager.receiveCommand("in", rejected);
    REQUIRE(mock->received.back().status == "rejected");
    REQUIRE(mock->received.back().detail == "driver unavailable");
    manager.receiveCommand("in", command("cmd-3"));
    REQUIRE(mock->received.back().status == "accepted"); // completed cmd-2 evicted, active cmd-1 retained
    std::ostringstream metrics;
    manager.writeMetrics(metrics);
    REQUIRE(metrics.str().find("\"duplicate_commands\":1") != std::string::npos);
    manager.stop();
}

TEST_CASE("Northbound command tracking stays bounded when every slot is active", "[northbound]") {
    northbound::NorthboundManager manager(1);
    auto adapter = std::make_unique<MockAdapter>("in");
    auto* mock = adapter.get();
    REQUIRE(manager.add(std::move(adapter)));
    manager.setCommandHandler([](edge::UnifiedMessageV2&) { return std::string{}; });
    REQUIRE(manager.start());
    manager.receiveCommand("in", command("active"));
    manager.receiveCommand("in", command("overflow"));
    REQUIRE(mock->received.back().status == "rejected");
    REQUIRE(mock->received.back().detail == "command tracking capacity exceeded");
    manager.stop();
}

TEST_CASE("Synchronous driver result follows admission acknowledgement", "[northbound]") {
    northbound::NorthboundManager manager;
    auto adapter = std::make_unique<MockAdapter>("in");
    auto* mock = adapter.get();
    REQUIRE(manager.add(std::move(adapter)));
    manager.setCommandHandler([&](edge::UnifiedMessageV2& request) {
        auto response = request;
        response.status = "ok";
        response.quality = iot::Quality::good;
        REQUIRE(manager.completeCommand(response));
        return std::string{};
    });
    REQUIRE(manager.start());
    manager.receiveCommand("in", command("instant"));
    REQUIRE(mock->received.size() == 2);
    REQUIRE(mock->received[0].commandState == "accepted");
    REQUIRE(mock->received[1].commandState == "succeeded");
    manager.stop();
}

TEST_CASE("Northbound command results are correlated, terminal and timeout-aware", "[northbound]") {
    northbound::NorthboundManager manager;
    auto adapter = std::make_unique<MockAdapter>("in");
    auto* mock = adapter.get();
    REQUIRE(manager.add(std::move(adapter)));
    manager.setCommandHandler([](edge::UnifiedMessageV2&) { return std::string{}; });
    REQUIRE(manager.start());
    manager.receiveCommand("in", command("success"));
    auto result = command("success");
    result.status = "ok";
    result.quality = iot::Quality::good;
    result.detail = "write confirmed";
    REQUIRE(manager.completeCommand(result));
    REQUIRE(mock->received.back().status == "ok");
    REQUIRE(mock->received.back().commandState == "succeeded");
    REQUIRE(mock->received.back().correlationId == "success");
    REQUIRE_FALSE(manager.completeCommand(result));
    manager.receiveCommand("in", command("failure"));
    result.correlationId = "failure";
    result.status = "failed";
    result.quality = iot::Quality::unavailable;
    REQUIRE(manager.completeCommand(result));
    REQUIRE(mock->received.back().status == "failed");
    manager.receiveCommand("in", command("driver-reject"));
    result.correlationId = "driver-reject";
    result.status = "rejected";
    REQUIRE(manager.completeCommand(result));
    REQUIRE(mock->received.back().status == "rejected");
    REQUIRE(mock->received.back().commandState == "rejected");
    manager.receiveCommand("in", command("timeout"));
    result.correlationId = "timeout";
    result.status = "timeout";
    result.quality = iot::Quality::timeout;
    REQUIRE(manager.completeCommand(result));
    REQUIRE(mock->received.back().status == "timeout");
    auto deadline = command("manager-deadline");
    deadline.deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(10);
    manager.receiveCommand("in", deadline);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    manager.expireCommands();
    REQUIRE(mock->received.back().status == "timeout");
    REQUIRE(mock->received.back().correlationId == "manager-deadline");
    result.correlationId = "manager-deadline";
    result.status = "ok";
    REQUIRE_FALSE(manager.completeCommand(result));
    manager.stop();
}

TEST_CASE("MQTT adapter owns topic and serialization compatibility", "[northbound]") {
    northbound::MqttNorthboundAdapter local({});
    edge::UnifiedMessageV2 message;
    message.deviceId = "motor";
    message.pointId = "speed";
    message.correlationId = "cmd-42";
    REQUIRE(local.topicFor(message) == "device/motor/telemetry");
    REQUIRE(local.serialize(message).find("\"device_id\":\"motor\"") != std::string::npos);
    message.northboundType = edge::NorthboundType::attribute;
    REQUIRE(local.topicFor(message) == "device/motor/attribute/speed");
    message.northboundType = edge::NorthboundType::status;
    REQUIRE(local.topicFor(message) == "device/motor/status");
    message.northboundType = edge::NorthboundType::alarm;
    REQUIRE(local.topicFor(message) == "gateway/mqmgateway-iot/alarm");
    message.pointId = "health/device-1";
    message.operation = "raised";
    message.status = "critical";
    message.eventSequence = 7;
    message.eventEpochMs = 123456;
    REQUIRE(local.serialize(message) ==
        "{\"sequence\":7,\"key\":\"health/device-1\",\"action\":\"raised\","
        "\"severity\":\"critical\",\"epoch_ms\":123456}");
    message.pointId = "health/bad\"\nname";
    REQUIRE(local.serialize(message).find("health/bad\\\"\\u000aname") != std::string::npos);
    message.northboundType = edge::NorthboundType::diagnostic;
    REQUIRE(local.topicFor(message) == "gateway/mqmgateway-iot/diagnostics");
    message.northboundType = edge::NorthboundType::command;
    REQUIRE(local.topicFor(message) == "device/motor/command");
    message.northboundType = edge::NorthboundType::command_result;
    message.status = "succeeded";
    REQUIRE(local.topicFor(message) == "device/motor/status");
    REQUIRE(local.serialize(message).find("\"command_id\":\"cmd-42\"") != std::string::npos);
    const auto parsed = local.parseCommand("device/can0/cmd/can_tx",
        R"({"can_id":291,"data":"0102","command_id":"parsed-17","timeout_ms":2000})");
    REQUIRE(parsed.error.empty());
    REQUIRE(parsed.message.correlationId == "parsed-17");
    REQUIRE(parsed.message.timeout == std::chrono::milliseconds(2000));
    REQUIRE(parsed.message.operation == "can_tx");
    REQUIRE(parsed.message.rawPayload == std::vector<std::uint8_t>{1, 2});
    REQUIRE_FALSE(local.parseCommand("device/can0/cmd/can_tx", R"({"data":"bad"})").error.empty());
    REQUIRE_FALSE(local.parseCommand("device/can0/cmd/can_tx",
        R"({"can_id":1,"data":"00","deadline_ms":18446744073709551615})").error.empty());
    northbound::MqttAdapterConfig cloudConfig;
    cloudConfig.cloudMode = true;
    northbound::MqttNorthboundAdapter cloud(std::move(cloudConfig));
    REQUIRE(cloud.topicFor(message) == "resume/gateway/devices/motor/command/result");
}

TEST_CASE("MQTT adapter rejects overload instead of growing its outbound queue", "[northbound]") {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(listener >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    REQUIRE(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener, 1) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length) == 0);
    std::atomic<bool> done{false};
    std::thread fakeBroker([&] {
        int peer = -1;
        while (!done) {
            fd_set ready;
            FD_ZERO(&ready);
            FD_SET(listener, &ready);
            timeval timeout{0, 100000};
            if (::select(listener + 1, &ready, nullptr, nullptr, &timeout) > 0) {
                peer = ::accept(listener, nullptr, nullptr);
                break;
            }
        }
        while (!done) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (peer >= 0) ::close(peer);
    });
    northbound::MqttAdapterConfig config;
    config.id = "bounded-offline";
    config.clientId = "bounded-offline-test";
    config.port = ntohs(address.sin_port); // accepts TCP but never sends MQTT CONNACK
    config.outboundCapacity = 1;
    northbound::MqttNorthboundAdapter adapter(std::move(config));
    bool started = false;
    try { started = adapter.start(); } catch (...) {}
    edge::UnifiedMessageV2 telemetry;
    telemetry.deviceId = "d1";
    unsigned rejected = 0;
    if (started) {
        for (unsigned i = 0; i < 100; ++i)
            if (!adapter.publish(telemetry).accepted) ++rejected;
    }
    const auto snapshot = adapter.metrics();
    adapter.stop();
    done = true;
    fakeBroker.join();
    ::close(listener);
    REQUIRE(started);
    REQUIRE(rejected > 0);
    REQUIRE(snapshot.queuePeak <= 1);
    REQUIRE(snapshot.dropped >= rejected);
}

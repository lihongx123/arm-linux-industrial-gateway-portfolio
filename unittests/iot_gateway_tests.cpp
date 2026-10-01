#include "bounded_queue.hpp"
#include "command_router.hpp"
#include "unified_message.hpp"

#include <catch2/catch_test_macros.hpp>

using namespace mqmgateway::iot;

TEST_CASE("Unified CAN telemetry serializes all required fields") {
    UnifiedMessage message;
    message.deviceId = "can-291";
    message.address = 291;
    message.payload = {0x01, 0xA2, 0xFF};
    const auto json = toJson(message);
    REQUIRE(json.find("\"device_id\":\"can-291\"") != std::string::npos);
    REQUIRE(json.find("\"protocol\":\"can\"") != std::string::npos);
    REQUIRE(json.find("\"payload\":\"01a2ff\"") != std::string::npos);
    REQUIRE(json.find("\"quality\":\"good\"") != std::string::npos);
}

TEST_CASE("CAN MQTT command is validated and routed") {
    CommandRouter router;
    const auto valid = router.route("device/can0/cmd/can_tx", R"({"can_id":291,"data":"0102a0","timeout_ms":500})");
    REQUIRE(valid.accepted);
    REQUIRE(valid.message.protocol == Protocol::can);
    REQUIRE(valid.message.direction == Direction::southbound);
    REQUIRE(valid.message.address == 291);
    REQUIRE(valid.message.payload == std::vector<std::uint8_t>{0x01, 0x02, 0xA0});

    REQUIRE_FALSE(router.route("bad/topic", "{}").accepted);
    REQUIRE_FALSE(router.route("device/can0/cmd/nope", "{}").accepted);
    REQUIRE_FALSE(router.route("device/can0/cmd/can_tx", R"({"can_id":1,"data":"xyz"})").accepted);
    REQUIRE_FALSE(router.route("device/can0/cmd/can_tx", R"({"can_id":1,"data":"0g"})").accepted);
    REQUIRE_FALSE(router.route("device/can0/cmd/can_tx", R"({"can_id":536870912,"data":"00"})").accepted);
}

TEST_CASE("Bounded queue rejects overflow and exposes metrics") {
    BoundedQueue<int> queue(2);
    REQUIRE(queue.tryPush(1));
    REQUIRE(queue.tryPush(2));
    REQUIRE_FALSE(queue.tryPush(3));
    REQUIRE(queue.pop() == 1);
    const auto metrics = queue.metrics();
    REQUIRE(metrics.enqueued == 2);
    REQUIRE(metrics.dequeued == 1);
    REQUIRE(metrics.rejected == 1);
    REQUIRE(metrics.currentDepth == 1);
    REQUIRE(metrics.peakDepth == 2);
    queue.stop();
}

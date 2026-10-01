#include "pipeline_metrics.hpp"
#include "gateway.hpp"
#include <catch2/catch_test_macros.hpp>
#include <rapidjson/document.h>
#include <sstream>
#include <thread>

using namespace mqmgateway::iot;

namespace {
rapidjson::Document snapshot(const PublishTracker& tracker) {
    std::ostringstream output;
    tracker.write(output);
    rapidjson::Document doc;
    doc.Parse(output.str().c_str());
    REQUIRE_FALSE(doc.HasParseError());
    return doc;
}
}

TEST_CASE("Pipeline QoS1 acceptance is distinct from PUBACK", "[pipeline]") {
    PublishTracker tracker;
    REQUIRE(tracker.submit(1, true, [](int* mid) { *mid = 7; return 0; }) == 0);
    auto before = snapshot(tracker);
    REQUIRE(before["publish_accepted"].GetUint64() == 1);
    REQUIRE(before["puback_received"].GetUint64() == 0);
    REQUIRE(before["pending_tracked"].GetUint64() == 1);
    std::thread ack([&] { tracker.completed(7); });
    ack.join();
    auto after = snapshot(tracker);
    REQUIRE(after["pending_tracked"].GetUint64() == 0);
    REQUIRE(after["telemetry_puback_received"].GetUint64() == 1);
    REQUIRE(after["telemetry_puback"]["count"].GetUint64() == 1);
    REQUIRE(after["tracking_complete"].GetBool());
}

TEST_CASE("Pipeline synchronous QoS0 callback is not a PUBACK", "[pipeline]") {
    PublishTracker tracker;
    REQUIRE(tracker.submit(0, true, [&](int* mid) {
        *mid = 42;
        tracker.completed(42);
        return 0;
    }) == 0);
    auto doc = snapshot(tracker);
    REQUIRE(doc["qos0_local_completed"].GetUint64() == 1);
    REQUIRE(doc["puback_received"].GetUint64() == 0);
    REQUIRE(doc["pending_tracked"].GetUint64() == 0);
    REQUIRE(doc["tracking_complete"].GetBool());
}

TEST_CASE("Pipeline limits memory and marks incomplete correlation", "[pipeline]") {
    PublishTracker tracker(1);
    tracker.submit(1, true, [](int* mid) { *mid = 1; return 0; });
    tracker.submit(1, true, [](int* mid) { *mid = 2; return 0; });
    tracker.completed(2);
    tracker.submit(1, true, [](int*) { return 4; });
    auto doc = snapshot(tracker);
    REQUIRE(doc["pending_tracked"].GetUint64() == 1);
    REQUIRE(doc["tracking_dropped"].GetUint64() == 1);
    REQUIRE(doc["publish_api_errors"].GetUint64() == 1);
    REQUIRE(doc["unmatched_callbacks"].GetUint64() == 1);
    REQUIRE_FALSE(doc["tracking_complete"].GetBool());
}

TEST_CASE("Pipeline MID reuse is reported instead of false ACK matching", "[pipeline]") {
    PublishTracker tracker;
    tracker.submit(1, true, [](int* mid) { *mid = 1; return 0; });
    tracker.submit(1, true, [](int* mid) { *mid = 1; return 0; });
    auto doc = snapshot(tracker);
    REQUIRE(doc["tracking_dropped"].GetUint64() == 2);
    REQUIRE_FALSE(doc["tracking_complete"].GetBool());
}

TEST_CASE("Pipeline latency histogram reports bounded quantiles", "[pipeline]") {
    StageLatency latency;
    for (int i = 1; i <= 100; ++i) latency.observe(std::chrono::microseconds(i));
    std::ostringstream output;
    latency.write(output);
    rapidjson::Document doc;
    doc.Parse(output.str().c_str());
    REQUIRE(doc["count"].GetUint64() == 100);
    REQUIRE(doc["p50_upper_us"].GetDouble() == 64.0);
    REQUIRE(doc["p99_upper_us"].GetDouble() == 100.0);
    REQUIRE(doc["mean_us"].GetDouble() == 50.5);
}

TEST_CASE("Pipeline configuration keeps safe defaults and rejects unbounded windows", "[pipeline]") {
    GatewayConfig config;
    REQUIRE(config.telemetryQos == 1);
    REQUIRE(config.mqttMaxInflight == 20);
    REQUIRE_FALSE(config.pipelineMetrics);
    config.mqttMaxInflight = 0;
    REQUIRE_THROWS_AS(Gateway{config}, std::invalid_argument);
    config.mqttMaxInflight = 65536;
    REQUIRE_THROWS_AS(Gateway{config}, std::invalid_argument);
    config.mqttMaxInflight = 100;
    config.telemetryQos = 2;
    REQUIRE_THROWS_AS(Gateway{config}, std::invalid_argument);
}

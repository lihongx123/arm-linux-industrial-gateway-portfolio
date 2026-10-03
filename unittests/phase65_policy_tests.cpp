#include "telemetry_policy.hpp"
#include "bounded_queue.hpp"
#include "diagnostics.hpp"
#include <catch2/catch_test_macros.hpp>
#include <atomic>
#include <thread>
#include <vector>

using namespace mqmgateway;

TEST_CASE("Phase 6.5 reporting policy preserves default and compares last published numeric value", "[phase65]") {
    edge::TelemetryPolicy policy;
    edge::UnifiedMessageV2 message;
    message.deviceId = "d";
    message.pointId = "p";
    message.cookedValue = "10";
    const auto t0 = std::chrono::steady_clock::time_point{};
    int enqueued = 0;
    auto emit = [&] { ++enqueued; return true; };
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::published);
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::published);
    REQUIRE(enqueued == 2);
    REQUIRE(policy.configure("d", "p", {true, 2.0, std::chrono::milliseconds(100)}));
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::published);
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::suppressed);
    message.cookedValue = "11.9";
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::suppressed);
    message.cookedValue = "12";
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::published);
    message.cookedValue = "13";
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::suppressed);
    message.cookedValue = "14.1";
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0) == edge::TelemetryPolicy::Decision::published);
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0 + std::chrono::milliseconds(99)) == edge::TelemetryPolicy::Decision::suppressed);
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0 + std::chrono::milliseconds(100)) == edge::TelemetryPolicy::Decision::published);
    REQUIRE(policy.metrics().forcedMaxInterval == 1);
    message.quality = iot::Quality::timeout;
    REQUIRE(policy.process(message, edge::PointValueType::integer, emit, t0 + std::chrono::milliseconds(101)) == edge::TelemetryPolicy::Decision::published);
    REQUIRE_FALSE(policy.configure("d", "bad", {true, -1.0, std::chrono::milliseconds(0)}));
}

TEST_CASE("Phase 6.5 boolean text and failed enqueue keep correct baseline", "[phase65]") {
    edge::TelemetryPolicy policy;
    REQUIRE(policy.configure("d", "flag", {true, 0, std::chrono::milliseconds(0)}));
    REQUIRE(policy.configure("d", "text", {true, 0, std::chrono::milliseconds(0)}));
    edge::UnifiedMessageV2 message;
    message.deviceId = "d";
    message.pointId = "flag";
    message.cookedValue = "false";
    int accepted = 0;
    auto emit = [&] { ++accepted; return true; };
    REQUIRE(policy.process(message, edge::PointValueType::boolean, emit) == edge::TelemetryPolicy::Decision::published);
    REQUIRE(policy.process(message, edge::PointValueType::boolean, emit) == edge::TelemetryPolicy::Decision::suppressed);
    message.cookedValue = "true";
    REQUIRE(policy.process(message, edge::PointValueType::boolean, emit) == edge::TelemetryPolicy::Decision::published);
    message.pointId = "text";
    message.cookedValue = "a";
    REQUIRE(policy.process(message, edge::PointValueType::text, [] { return false; }) == edge::TelemetryPolicy::Decision::queueRejected);
    REQUIRE(policy.process(message, edge::PointValueType::text, emit) == edge::TelemetryPolicy::Decision::published);
    message.cookedValue = "b";
    REQUIRE(policy.process(message, edge::PointValueType::text, emit) == edge::TelemetryPolicy::Decision::published);
    REQUIRE(policy.stateSize() == 2);
}

TEST_CASE("Phase 6.5 suppressed samples still refresh diagnostics and concurrent policy state is bounded", "[phase65]") {
    edge::DiagnosticsManager diagnostics({1, 1, std::chrono::milliseconds(1000)});
    REQUIRE(diagnostics.registerDevice("d"));
    edge::TelemetryPolicy policy;
    REQUIRE(policy.configure("d", "p", {true, 0, std::chrono::milliseconds(0)}));
    edge::UnifiedMessageV2 sample;
    sample.deviceId = "d";
    sample.pointId = "p";
    sample.cookedValue = "7";
    std::atomic<int> accepted{0};
    std::vector<std::thread> threads;
    for (int n = 0; n < 8; ++n) threads.emplace_back([&] {
        for (int i = 0; i < 100; ++i) {
            diagnostics.observe(sample);
            policy.process(sample, edge::PointValueType::integer, [&] { ++accepted; return true; });
        }
    });
    for (auto& thread : threads) thread.join();
    REQUIRE(accepted == 1);
    REQUIRE(policy.metrics().suppressedCov == 799);
    REQUIRE(policy.stateSize() == 1);
    REQUIRE(diagnostics.health("d")->state == edge::HealthState::online);
}

TEST_CASE("Phase 6.5 independent bounded queues survive inverse pressure and clean shutdown", "[phase65]") {
    iot::BoundedQueue<edge::UnifiedMessageV2> telemetry(4), commands(4);
    edge::UnifiedMessageV2 sample;
    for (int n = 0; n < 4; ++n) REQUIRE(telemetry.tryPush(sample));
    REQUIRE_FALSE(telemetry.tryPush(sample));
    REQUIRE(commands.tryPush(sample));
    REQUIRE(commands.pop().has_value());
    REQUIRE(telemetry.metrics().currentDepth == 4);
    for (int n = 0; n < 4; ++n) REQUIRE(commands.tryPush(sample));
    REQUIRE_FALSE(commands.tryPush(sample));
    REQUIRE(telemetry.pop().has_value());
    telemetry.stop();
    commands.stop();
    while (telemetry.pop()) {}
    while (commands.pop()) {}
    REQUIRE_FALSE(telemetry.pop().has_value());
    REQUIRE_FALSE(commands.pop().has_value());
}

TEST_CASE("Phase 6.5 policy configuration cannot exceed registry point limit", "[phase65]") {
    edge::TelemetryPolicy policy;
    bool accepted = true;
    for (int n = 0; n < 65536; ++n)
        accepted = policy.configure("d", std::to_string(n), {true, 0, std::chrono::milliseconds(0)}) && accepted;
    REQUIRE(accepted);
    REQUIRE_FALSE(policy.configure("d", "overflow", {true, 0, std::chrono::milliseconds(0)}));
}

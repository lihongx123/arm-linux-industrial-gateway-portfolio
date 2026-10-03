# Phase 6.5 implementation record

## Source audit and plan (before implementation)

GatewayCore validates device/point ownership, maps values, and updates Diagnostics before calling Gateway's publication callback. Gateway currently enqueues telemetry and admitted commands into one bounded queue and uses one worker pool. Admission is reported only after enqueue succeeds. Workers recheck command deadlines before submit; a correlated status result bypasses that queue and completes through NorthboundManager. Stopping the queue wakes its workers. One QueueWatchdog observes its dequeue progress. MQTT has a separate bounded outbound FIFO and sender thread; that FIFO does not isolate Gateway command execution. Point registration is partly static and partly dynamic through driver descriptions.

Implementation order:

1. Add a protocol-neutral, bounded and thread-safe reporting policy after mapping/diagnostics. Commit the last-published baseline only after bounded telemetry enqueue succeeds. Keep PointMapper stateless and preserve publish-every-sample by default.
2. Split Gateway into bounded telemetry and command queues with independent workers, wait metrics, shutdown, and watchdog observations. Keep legacy queue arguments as telemetry aliases.
3. Test adapter saturation. Reserve bounded outbound control capacity if telemetry can crowd out command results and alarms.
4. Add deterministic unit/integration coverage, run full local CTest, and save fresh evidence without replacing Phase 1-6 results.

No legacy modmqttd changes, dependency installation, cloud, physical hardware, or ARM64 rerun are part of this phase.

## Runtime data path and policy

GatewayCore validates the device and point, applies PointMapper, updates Diagnostics/DeviceHealth, then invokes Gateway's callback. The callback applies TelemetryPolicy only to telemetry. A suppressed sample still refreshes health. Status is not suppressed; a correlated driver completion still goes straight to NorthboundManager. No protocol-specific COV branch exists.

Configure repeatedly with `--point-policy=device_id,point_id,cov,deadband,max_report_ms`; for example `--point-policy=motor,rpm,1,2.5,1000`. COV accepts 0/1 or true/false. Deadband must be finite and nonnegative; interval is an integer 0–3,600,000 ms. Policies may name a dynamic point not yet registered: they remain pending by exact device/point key and apply when that point appears. The policy table and active state table each have at most 65,536 entries. A policy for a point that never appears is inert, not an implicit new point. Non-numeric points ignore deadband and compare mapped values exactly. Floating wire mapping remains unsupported by PointMapper.

Without an explicit policy, every valid telemetry sample attempts bounded enqueue. With COV, the first sample publishes; boolean/text/bytes publish on exact change, numeric points compare parsed finite mapped values against the last successfully *enqueued* value. A zero deadband requires any numeric change; a positive deadband uses an inclusive threshold. A quality change always publishes. A positive max-report interval forces publication from a steady-clock deadline even if unchanged. The callback holds the policy mutex only while deciding and trying the local bounded queue insertion, never during MQTT/network I/O. Rejected enqueue does not advance the baseline. “Published” in this policy means *accepted by the Gateway telemetry queue*, not broker PUBACK.

## Queue topology and compatibility

Previously one Gateway bounded queue and worker pool carried both telemetry and commands. Now telemetryQueue and commandQueue have independent capacities, workers and wait metrics. `--queue-capacity` remains the legacy telemetry capacity (default 1024); `--telemetry-queue-capacity` is its explicit alias, with the last supplied option winning. `--workers` remains telemetry workers (default 2). New `--command-queue-capacity` defaults to 64 and `--command-workers` defaults to 1. A full command queue returns “command queue capacity exceeded” before admission is reported accepted. Command workers recheck deadline and preserve submit/final-result routing. Both pools make progress independently and stop via queue wakeup. The queue full path never waits on a southbound reactor thread.

The MQTT adapter's outbound queue is a *second* bound, not the Gateway work queue. `--mqtt-outbound-capacity` optionally sets that bound explicitly; absent it, the historical `--queue-capacity` value also remains the MQTT bound. Within this total, at least 1/4 (minimum 1 slot) is reserved for control, while the rest belongs to routine messages. CommandResult, critical Status, Alarm/Ack are control; telemetry, gateway heartbeat and periodic diagnostic snapshot are routine. Lane choice does not change the message's existing MQTT QoS. At total capacity 1, routine messages can use the single slot when idle; a new control item evicts an unsent routine item (counted in dropped) and takes that slot. The sender serves at most three consecutive control messages before one waiting routine message. This prevents telemetry alone from exhausting control capacity but does not guarantee delivery during disconnection or control-plane overload. Failed control enqueue is visible through dropped/command-result-failure metrics.

## Observability and exact success semantics

New gateway JSON keys include telemetry_samples_valid, telemetry_published, telemetry_suppressed_cov, telemetry_forced_max_interval, telemetry_mapping_failed, telemetry_queue_rejected, command_queue_rejected, and separate telemetry_queue/command_queue metrics (depth, peak, enqueued, dequeued, rejected, mean wait). The original top-level queue fields still alias telemetry for older tools. Watchdogs expose telemetry_queue_watchdog and command_queue_watchdog, with separate telemetry_queue_stalled/command_queue_stalled alarms. The old queue_watchdog metric and queue_stalled alarm are retained as telemetry aliases for existing Phase 6 consumers.

Queue accepted, Mosquitto publish API accepted, broker PUBACK, and physical device succeeded are four different states. Command timeout means confirmation was not received within deadline; it does not prove a driver write did not occur. Both new watchdogs report only; neither restarts the process.

## Phase 6.5 changed-file inventory

- Runtime: `src/edge_core/telemetry_policy.hpp`, `telemetry_policy.cpp`, `gateway_core.hpp`, `gateway_core.cpp`, `diagnostics.hpp`, `diagnostics.cpp`, `watchdog.hpp`, `watchdog.cpp`, `CMakeLists.txt`; `src/iot_gateway/gateway.hpp`, `gateway.cpp`, `main.cpp`; `src/northbound/mqtt_northbound_adapter.hpp`, `mqtt_northbound_adapter.cpp`.
- Tests/build: `unittests/phase65_policy_tests.cpp`, `northbound_tests.cpp`, `CMakeLists.txt`; `tests/integration/phase65_queue_isolation_test.py`.
- Documents: this file, `docs/GATEWAY_SOURCE_STUDY_GUIDE.md`, `docs/architecture.md`, `docs/northbound_architecture.md`, `docs/industrial_gateway_upgrade_plan.md`, `docs/EVIDENCE_INDEX.md`.
- New evidence only under `results/edge_core/phase6-5-20261003/`. Existing Phase 1–6 files and other uncommitted source edits in the worktree were preserved.

## Local evidence and limits

New raw logs and machine-readable outcomes are in [Phase 6.5 results](../results/edge_core/phase6-5-20261003/README.md). The first CTest attempt was 7/8: three legacy ExprTk tests failed because the new build directory lacked exprtk.hpp, while 181/184 unit cases passed. The existing header at /tmp/exprtk-src was reused without installation; the corrected full-option Release build then passed 11/11 CTest items, including the final one-slot compatibility rerun. Focused Phase 6.5 tests passed 6/6 cases and 56/56 assertions; combined Phase 6.5/northbound tests passed 14/14 cases and 162/162 assertions. A local Mosquitto/vcan queue saturation run observed 195 telemetry queue rejections and independent command admission/result; a 12-command burst returned 12 explicit terminal outcomes while telemetry continued. Existing command lifecycle and Phase 6 watchdog integrations also passed against the final binary. The evidence index above contains the exact timing and raw logs.

This validates local software only. The changed binary has not been revalidated in ARM64 Buildroot/QEMU, physical devices, or EMQX Cloud.

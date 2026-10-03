# Phase 6.5 local software validation

Date: 2026-10-03. This directory is new; earlier Phase 1–6 evidence has not been overwritten. Source remains uncommitted.

## Build and tests

- Initial optional-dependency-off build succeeded (`build.log`). First CTest (`ctest.log`) was 7/8: 181/184 unit cases passed; the 3 failures were existing ExprTk converter cases because `EXPRTK_INCLUDE_DIR-NOTFOUND` omitted `exprconv.so`. The failure log is retained.
- An existing local header at `/tmp/exprtk-src/exprtk.hpp` was reused; no dependency was installed. The corrected base run (`build-exprtk.log`, `ctest-exprtk.log`) passed 8/8.
- Final Release build enabled existing OPC UA and S7 dependencies and ExprTk (`build-full.log`, `build-full-final.log`, `build-final-tests.log`, `build-one-slot-fix.log`). The final complete CTest (`ctest-final-one-slot.log`) passed 11/11, 0 failed, in 185.06 s. Earlier passes are retained separately.
- Focused tests after the one-slot fix (`focused-final.log`) passed 14/14 cases, 162/162 assertions across Phase 6.5 and northbound tags. They cover default publish-all, first sample, numeric deadband boundary/last-published baseline, quality, max interval, boolean/text changes, rejected enqueue, concurrent suppression, diagnostics freshness, bounded policy table, independent queue shutdown and saturated adapter behavior.
- The existing local generic-TCP/Mosquitto command lifecycle regression passed against the final binary (`command-lifecycle-final.log`): success, duplicate rejection, expired timeout, invalid request rejection and 0 command-result publish failures.
- The existing Phase 6 vcan/Mosquitto watchdog/ack regression passed against the final full binary (`phase6-watchdog-final.log`).

## Local overload and comparison

The final vcan/Mosquitto run `queue-isolation-final/` and its log used telemetry capacity 4, command capacity 4, one worker each, processing delay 200 ms, MQTT outbound capacity 64, CAN COV for point `can-291/frame`, and a 200-frame burst. It passed: 195 telemetry queue rejections, peak telemetry depth 4, peak command depth 4, 4 repeated samples suppressed, first control admission 43.432 ms and terminal success 200.904 ms. Command burst caused 11 explicit command-queue rejections; all 12 burst IDs had terminal outcomes. Telemetry continued dequeuing during the command burst. This is synthetic local timing, not a production latency SLA.

For a narrow before/after demonstration, `legacy-comparison/` ran an earlier locally built shared-queue binary and `new-comparison-final/` ran the final split-queue binary. Both used 4 telemetry slots, one telemetry worker, 200 ms artificial delay, 200 CAN frames and an MQTT outbound bound of 4. The old binary rejected the command at 43.541 ms; the new binary admitted it at 44.640 ms and completed it at 200.950 ms. Both runs rejected 195 telemetry frames. The old binary SHA-256 was `6bbb8cb78e3ad01b5ae1c4d1da857ad3506c2a784ba5e4d99df2ff185e36b587`; the final new binary SHA-256 was `a7a5492632537bcb0b7551e4e414f9e21e4052904992bc1320919b9bde670793`. The earlier binary comes from an uncommitted local Phase 6 build, not an immutable tagged source release, so this comparison demonstrates the queue behavior rather than a reproducible product benchmark.

The final pass does not claim ARM64 Buildroot/QEMU, EMQX Cloud or physical hardware validation for this changed source. Queue acceptance, Mosquitto API acceptance, broker PUBACK and device success remain separate evidence states.

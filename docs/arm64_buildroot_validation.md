# ARM64 / Buildroot validation

This document records the no-hardware validation path for the gateway extension. It is intentionally separate from the baseline evidence: the baseline is the native build at commit `b6f24d3b2c4596a19082e584326df70adffedbb6`, while this validation cross-builds the same source for an ARM64 Buildroot image and executes it under QEMU.

## Scope

The requested gaps are covered by:

- ARM64 Buildroot image and QEMU `virt` boot (`qemu-system-aarch64`);
- CMake cross compilation through `cmake/toolchains/buildroot-aarch64.cmake`;
- direct C++ Linux termios transport in `src/serial/termios_rtu_transport.*`;
- RTU stream handling for fragmented, sticky/coalesced, noisy, CRC-invalid and continuously invalid input;
- guest MQTT/CAN functional, reconnect/fault and throughput scenarios;
- a repeatable long-running guest soak (`run-arm-soak.sh`, default 28,800 seconds / 8 hours).

No physical serial adapter, CAN interface or external hardware is used. The serial transport uses a kernel PTY pair and CAN uses an in-guest `vcan0` interface.

## Versions and reproducibility

- Buildroot: 2025.02.18 LTS, selected from the official Buildroot download page;
- target: `aarch64`, Linux kernel 6.12.27, QEMU `virt` machine;
- host QEMU package: Ubuntu 24.04 package 1:8.2.2+ds-0ubuntu1.18;
- Buildroot source SHA-256: `00c772f86a60db0dc726ccc1bed8b6c83b8aa516f799032557e0e59d33eed95c`;
- baseline commit: `b6f24d3b2c4596a19082e584326df70adffedbb6`;
- target packages: libmodbus, Mosquitto client/broker, fmt, spdlog, yaml-cpp, RapidJSON, can-utils, iproute2, socat, strace and GDB;
- kernel fragment enables `CONFIG_CAN`, `CONFIG_CAN_RAW`, `CONFIG_CAN_VCAN`, `CONFIG_CAN_DEV` and Unix PTYs.

## Commands

```text
bash scripts/build_buildroot_arm64.sh
python3 scripts/run_buildroot_qemu.py --mode tests
python3 scripts/run_buildroot_qemu.py --mode stress --stress-seconds 30 --stress-devices 100 --stress-queue 1024 --stress-workers 2
python3 scripts/run_buildroot_qemu.py --mode soak --soak-seconds 28800 --soak-rate 500 --soak-devices 100 --soak-queue 1024 --soak-workers 2
```

The build script writes complete logs under `results/arm64/buildroot/` and `results/arm64/cross-build/`. QEMU console and extracted guest files are under `results/arm64/qemu/`, `results/arm64/stress/` and `results/arm64/soak/`.

## Measured QEMU results

The ARM guest functional suite passed RTU stream parsing, CAN→MQTT, MQTT→CAN, invalid-command rejection and broker reconnect. The initial 500-message performance case delivered 500/500 with zero loss.

The rate ladder used 100 devices, two gateway workers and a queue capacity of 1024. CPU and RSS were read inside the Buildroot guest from the gateway PID's `/proc/<pid>/stat` and `/proc/<pid>/status`; Windows and whole-host utilization are not included.

| Target msg/s | Classification | Load-end receive msg/s | Loss | P99 | Gateway CPU | Peak RSS | Queue peak |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 100 | STABLE | 99.998 | 0 | 7.357 ms | 17.43% | 3.22 MB | 1 |
| 250 | STABLE | 249.887 | 0 | 8.079 ms | 37.62% | 3.19 MB | 2 |
| 500 | STABLE | 499.983 | 0 | 34.928 ms | 50.20% | 3.31 MB | 6 |
| 1000 | STABLE | 999.944 | 0 | 81.623 ms | 69.71% | 3.31 MB | 13 |
| 1500 | DEGRADED (30 s) | 1499.447 | 0 | 276.906 ms | 77.37% | 3.34 MB | 38 |
| 2000 | FAIL | 1694.904 | 2.2033% | 4574.657 ms | 68.45% | 6.14 MB | 31 |

Two 60-second repetitions at 1500 msg/s both crossed the failure threshold: one reached P99 1257.3 ms, while the second fell to 867.0 msg/s during load with 32.5763% final loss and P99 25.296 s. Therefore 1000 msg/s is the highest demonstrated short stable step, 1500 msg/s is not stable over longer repetitions, and 2000 msg/s is a clear overload point in this QEMU configuration.

The 8-hour run deliberately used 500 msg/s to retain operating headroom. It completed with `guest_exit=PASS`, `debugfs_exit=0` and these guest measurements:

- requested/actual load duration: 28,800 / 28,800.000184 seconds;
- sent/received: 14,400,000 / 14,400,000, loss 0, malformed 0;
- actual send and load-end receive rate: 499.999997 / 499.999997 msg/s;
- P50/P95/P99: 2.4 / 7.5 / 52.7 ms;
- gateway-process CPU: 50.138090%; peak RSS: 3.261719 MB;
- queue peak: 13 of 1024; rejected: 0; publish failures: 0;
- command timeouts: 0; CAN errors: 0; gateway remained alive;
- total QEMU host-run time: 28,814.193 seconds.

Complete structured evidence is in `results/arm64/soak/guest-results/soak-results/`, with host execution metadata in `results/arm64/soak/host-run.txt`.

## Preserved harness incidents

The first long-run attempt is retained under `results/arm64/soak-failed-nospace-20260921/`. It was invalidated because an unbounded MQTT message evidence file grew to 347,248,632 bytes and exhausted the 384 MiB guest filesystem during final evidence collection. This was classified as a harness failure, not a gateway result. The harness was changed to constant-space collection and successfully regressed.

A later 120-second preflight with an incorrectly initialized latency histogram is retained under `results/arm64/soak-histogram-invalid-120s-20260921/`. Its zero latency percentiles are explicitly invalid. The corrected fixed-size histogram passed a 60-second preflight before the final 8-hour run.

A result is called PASS only when the guest command returns zero, the host extracts the guest evidence successfully, and the structured result duration and parameters match the requested run.

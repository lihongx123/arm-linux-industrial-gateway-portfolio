# ADC/PWM software simulation and ARM64 guest evidence

The new `mqmgateway_board_adc_pwm_tests` exercises the production ADC/PWM backend classes through `GatewayCore` and `AcquisitionScheduler` against deterministic, test-only file attributes. It verifies ADC raw 2048 → mapped 1014, PWM period 1,000,000 ns and duty 250,000 ns, range rejection, negative ADC rejection, and PWM safe-off on stop.

| Run | Result | Evidence |
| --- | --- | --- |
| Native Release first full regression | 12/12 CTest PASS, 185.67 s | `native-ctest.log` (local raw log) |
| Native Release final regression | 12/12 CTest PASS, 185.20 s | `native-build-final.log`, `native-ctest-final.log` (local raw logs), [summary.json](summary.json) |
| ARM64 cross-build | PASS with existing Buildroot toolchain | `arm64-build-final.log` (local raw log) |
| ARM64 Buildroot/QEMU final | PASS, QEMU 8.2.2, 34.647 s host elapsed | [host-run.txt](arm64-qemu-final/host-run.txt), [guest ADC/PWM JSON](arm64-qemu-final/guest-results/results/board-adc-pwm-probe.json), [injected binary hashes](arm64-qemu-final/injected-binaries.json) |

The first ARM64 reconfigure attempt omitted `BUILDROOT_OUTPUT_DIR` and failed before compilation. Re-running with `BUILDROOT_OUTPUT_DIR=/tmp/mqmgateway-br-output` used the pre-existing completed Buildroot output and passed. The first guest probe also passed; the `arm64-qemu-final/` run is against the final backend change and is the result to cite.

The guest does not emulate real ADC voltage or PWM pin output. No electrical measurements or physical board validation are claimed.

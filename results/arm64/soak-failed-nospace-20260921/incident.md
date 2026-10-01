# ARM64/QEMU 8-hour soak infrastructure incident

## Classification

- Result: INVALID / infrastructure failure
- Product verdict: no PASS or FAIL assigned
- Environment: Buildroot 2025.02.18, Linux 6.12.27, AArch64 QEMU
- Requested duration: 28,800 seconds
- Observed host runtime before termination: approximately 8 hours

## Symptom

The guest completed the long-running workload interval but could not persist the
final metrics and summary. The console ended with:

```text
cp: write error: No space left on device
```

The unmodified evidence is retained in `qemu-console.log` in this directory.

## Root cause

The soak harness wrote every MQTT telemetry payload to
`/root/soak-results/messages.jsonl`. Debugfs inspection of the failed 384 MiB
runtime image showed that this file had grown to 347,248,632 bytes. The test
harness therefore exhausted the guest root filesystem while attempting to copy
the final evidence. This was caused by unbounded test-result collection, not by
a demonstrated gateway crash or protocol failure.

## Correction

The soak harness now streams MQTT output through a FIFO and stores only the
received-message count. Evidence size is constant with respect to test duration.
The QEMU runner also records EOF/timeout failures in `host-run.txt` and attempts
result extraction instead of exiting without a host summary.

## Regression check

A 120-second ARM64/QEMU soak completed successfully after the correction. Its
complete evidence was archived separately before starting the replacement
8-hour run.

# User-requested interruption — 2026-09-22

The user requested stopping the 8-day run to prioritize bottleneck investigation.
The exact QEMU PID 786437 was checked and sent SIGTERM. The runner extracted
available evidence and exited. Host elapsed time: 15930.180 seconds (about 4h25m).

Classification: **USER_INTERRUPTED / INCOMPLETE**, neither product PASS nor product FAIL.
The unmodified runner reports `guest_exit=FAIL`, `failure_reason=EOF` because the
guest did not return a completed result; retain these raw values for audit.
No final duration, throughput, loss or latency result is available for this run.
Guest `/tmp/soak-metrics.json` was not present in the disk image after termination
(guest temporary state was not persisted). Do not invent partial counters.

The interrupted rootfs was preserved locally at
`/tmp/mqmgateway-1500-8x24h-interrupted-20260922.ext4` before reusing any runtime image.
The hourly automation was deleted. Earlier completed runs remain unchanged.

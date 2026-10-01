# Configuration-only ARM64 scalability results

QoS1, same gateway/probe binaries. CPU is guest process CPU, one core = 100%.
Loss is delivery-count deficit after at most 5 s drain, without sequence deduplication.

| Profile | Target | Actual send/s | Load receive/s | Final lost | P99 ms | Gateway CPU% | Broker CPU%* | Probe CPU%* | RSS MiB | Queue | Class |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| base | 4000 | 3999.99 | 3332.79 | 15062 | 9909.0 | 81.59 | 47.52 | 57.24 | 15.77 | 112 | FAIL |
| cpu4 | 4000 | 3999.99 | 3611.96 | 21890 | 562.3 | 134.85 | 89.93 | 103.19 | 3.46 | 34 | FAIL |
| cpu8 | 4000 | 3999.98 | 1805.90 | 112882 | 33210.6 | 171.03 | 99.92 | 113.30 | 43.27 | 22 | FAIL |

*Broker/probe CPU use the recorded 5-second sampling span; gateway CPU column uses the original probe load+drain interval. See JSON for comparable sampled gateway CPU and spans.
STABLE is the original probe threshold (loss <= 0.1%, receive/target >= 95%, P99 <= 100 ms, no malformed/rejected); it is not a zero-loss or long-duration certificate.
Sample overhead is present in all cases. Profile cpu4-workers4 is a two-factor interaction, not a single-factor attribution.

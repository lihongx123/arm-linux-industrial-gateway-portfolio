# Configuration-only ARM64 scalability results

QoS1, same gateway/probe binaries. CPU is guest process CPU, one core = 100%.
Loss is delivery-count deficit after at most 5 s drain, without sequence deduplication.

| Profile | Target | Actual send/s | Load receive/s | Final lost | P99 ms | Gateway CPU% | Broker CPU%* | Probe CPU%* | RSS MiB | Queue | Class |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| cpu8 | 3000 | 2999.99 | 1881.11 | 49764 | 22381.4 | 152.29 | 99.72 | 109.49 | 23.47 | 14 | FAIL |
| cpu8 | 4000 | 3999.98 | 1776.17 | 117418 | 34299.8 | 168.87 | 99.85 | 112.65 | 43.91 | 22 | FAIL |

*Broker/probe CPU use the recorded 5-second sampling span; gateway CPU column uses the original probe load+drain interval. See JSON for comparable sampled gateway CPU and spans.
STABLE is the original probe threshold (loss <= 0.1%, receive/target >= 95%, P99 <= 100 ms, no malformed/rejected); it is not a zero-loss or long-duration certificate.
Sample overhead is present in all cases. Profile cpu4-workers4 is a two-factor interaction, not a single-factor attribution.

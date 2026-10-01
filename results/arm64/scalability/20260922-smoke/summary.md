# Configuration-only ARM64 scalability results

QoS1, same gateway/probe binaries. CPU is guest process CPU, one core = 100%.
Loss is delivery-count deficit after at most 5 s drain, without sequence deduplication.

| Profile | Target | Actual send/s | Load receive/s | Final lost | P99 ms | Gateway CPU% | Broker CPU%* | Probe CPU%* | RSS MiB | Queue | Class |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| base | 1500 | 1499.92 | 1499.92 | 0 | 4.1 | 66.10 | 42.12 | 43.60 | 3.25 | 8 | STABLE |

*Broker/probe CPU use the recorded 5-second sampling span; gateway CPU column uses the original probe load+drain interval. See JSON for comparable sampled gateway CPU and spans.
STABLE is the original probe threshold (loss <= 0.1%, receive/target >= 95%, P99 <= 100 ms, no malformed/rejected); it is not a zero-loss or long-duration certificate.
Sample overhead is present in all cases. Profile cpu4-workers4 is a two-factor interaction, not a single-factor attribution.

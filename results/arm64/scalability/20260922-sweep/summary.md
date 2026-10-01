# Configuration-only ARM64 scalability results

QoS1, same gateway/probe binaries. CPU is guest process CPU, one core = 100%.
Loss is delivery-count deficit after at most 5 s drain, without sequence deduplication.

| Profile | Target | Actual send/s | Load receive/s | Final lost | P99 ms | Gateway CPU% | Broker CPU%* | Probe CPU%* | RSS MiB | Queue | Class |
| --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- |
| base | 4000 | 3999.99 | 3452.67 | 4634 | 8026.2 | 75.72 | 51.74 | 57.68 | 13.20 | 99 | FAIL |
| cpu4 | 4000 | 3999.99 | 3552.44 | 17629 | 2455.1 | 133.63 | 94.48 | 102.63 | 6.33 | 39 | FAIL |
| cpu4-workers4 | 4000 | 3999.98 | 3538.81 | 20469 | 1841.8 | 139.75 | 89.34 | 101.66 | 5.30 | 42 | FAIL |
| devices10 | 4000 | 3999.99 | 3300.36 | 13318 | 10413.0 | 80.80 | 49.19 | 56.70 | 16.24 | 67 | FAIL |
| devices1000 | 4000 | 4000.00 | 3310.55 | 15632 | 10175.6 | 81.33 | 48.29 | 57.04 | 16.51 | 56 | FAIL |
| memory2048 | 4000 | 3999.99 | 3195.44 | 18079 | 11717.8 | 79.58 | 49.88 | 56.58 | 17.89 | 101 | FAIL |
| memory512 | 4000 | 3999.70 | 3190.72 | 18791 | 11806.6 | 79.77 | 49.56 | 57.23 | 17.80 | 75 | FAIL |
| queue256 | 4000 | 3999.71 | 3492.50 | 7125 | 7485.7 | 78.02 | 49.55 | 58.24 | 12.22 | 53 | FAIL |
| queue4096 | 4000 | 3999.84 | 3069.47 | 29764 | 13339.1 | 80.42 | 49.70 | 56.12 | 19.90 | 112 | FAIL |
| workers1 | 4000 | 3999.97 | 3207.33 | 15044 | 10578.9 | 76.88 | 49.89 | 58.73 | 16.52 | 49 | FAIL |
| workers4 | 4000 | 3999.98 | 2723.82 | 51810 | 18992.8 | 85.74 | 43.82 | 56.08 | 26.83 | 46 | FAIL |
| workers8 | 4000 | 3999.91 | 2833.66 | 42658 | 17035.0 | 83.40 | 46.17 | 54.92 | 24.43 | 44 | FAIL |

*Broker/probe CPU use the recorded 5-second sampling span; gateway CPU column uses the original probe load+drain interval. See JSON for comparable sampled gateway CPU and spans.
STABLE is the original probe threshold (loss <= 0.1%, receive/target >= 95%, P99 <= 100 ms, no malformed/rejected); it is not a zero-loss or long-duration certificate.
Sample overhead is present in all cases. Profile cpu4-workers4 is a two-factor interaction, not a single-factor attribution.

# Diagnostic matrix (read raw case JSON for scope)

| Case | Class | Receive/s | Lost | P99 ms | CPU % | RSS MiB | Queue peak | Pending peak | ACK P99 upper us |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| rate-1000-qos-0-observe-1 | STABLE | 999.99 | 0 | 44.200 | 38.70 | 3.34 | 3 | 4 | 0 |
| rate-1000-qos-1-observe-0 | STABLE | 999.99 | 0 | 1.500 | 44.27 | 3.25 | 9 | N/A | N/A |
| rate-1000-qos-1-observe-1 | STABLE | 999.99 | 0 | 1.500 | 47.27 | 3.10 | 3 | 6 | 2048 |
| rate-1500-qos-0-observe-1 | STABLE | 1499.96 | 0 | 53.700 | 57.15 | 3.02 | 6 | 8 | 0 |
| rate-1500-qos-1-observe-0 | STABLE | 1499.96 | 0 | 2.300 | 66.78 | 3.34 | 5 | N/A | N/A |
| rate-1500-qos-1-observe-1 | STABLE | 1499.96 | 0 | 1.900 | 65.08 | 3.15 | 4 | 13 | 2048 |
| rate-2000-qos-0-observe-1 | STABLE | 1999.88 | 0 | 60.800 | 73.84 | 3.15 | 8 | 9 | 0 |
| rate-2000-qos-1-observe-0 | STABLE | 1999.95 | 0 | 2.900 | 77.44 | 3.10 | 6 | N/A | N/A |
| rate-2000-qos-1-observe-1 | STABLE | 1999.88 | 0 | 3.300 | 80.97 | 3.02 | 9 | 21 | 4096 |
| rate-3000-qos-0-observe-1 | STABLE | 2997.72 | 0 | 79.000 | 84.41 | 3.14 | 24 | 28 | 0 |
| rate-3000-qos-1-observe-0 | DEGRADED | 2997.72 | 0 | 473.600 | 76.75 | 3.59 | 28 | N/A | N/A |
| rate-3000-qos-1-observe-1 | STABLE | 2999.66 | 0 | 18.000 | 86.97 | 3.21 | 17 | 94 | 31481.7 |
| rate-5000-qos-0-observe-1 | DEGRADED | 4991.06 | 248 | 79.800 | 77.19 | 3.34 | 431 | 393 | 0 |
| rate-5000-qos-1-observe-0 | FAIL | 3349.10 | 20660 | 8931.600 | 77.07 | 17.07 | 47 | N/A | N/A |
| rate-5000-qos-1-observe-1 | FAIL | 3171.26 | 28750 | 10294.600 | 79.16 | 18.75 | 51 | 4096 | 10123000.0 |

ACK histogram quantiles are bucket upper bounds. QoS0 has no PUBACK.
pending_tracked is not wire inflight. Incomplete tracking cannot establish exact conservation.
Original probe counts deliveries without sequence deduplication; observed loss is not a guaranteed unique-message loss measure.

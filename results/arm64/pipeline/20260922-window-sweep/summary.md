# Diagnostic matrix (read raw case JSON for scope)

| Case | Class | Receive/s | Lost | P99 ms | CPU % | RSS MiB | Queue peak | Pending peak | ACK P99 upper us |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| rate-3000-qos-1-observe-1-window-100 | STABLE | 2999.48 | 0 | 31.800 | 91.30 | 3.25 | 18 | 146 | 32768 |
| rate-3000-qos-1-observe-1-window-20 | DEGRADED | 2999.81 | 0 | 152.400 | 81.60 | 3.36 | 24 | 478 | 131072 |
| rate-3000-qos-1-observe-1-window-200 | STABLE | 2993.91 | 0 | 99.500 | 92.44 | 3.23 | 19 | 133 | 16384 |
| rate-5000-qos-1-observe-1-window-100 | FAIL | 3105.06 | 38648 | 5964.200 | 77.45 | 12.28 | 68 | 4096 | 5776660.0 |
| rate-5000-qos-1-observe-1-window-20 | FAIL | 3172.12 | 28477 | 9838.700 | 77.20 | 18.36 | 50 | 4096 | 9844200.0 |
| rate-5000-qos-1-observe-1-window-200 | FAIL | 3075.51 | 39476 | 5704.800 | 77.42 | 11.85 | 60 | 4096 | 5462470.0 |

ACK histogram quantiles are bucket upper bounds. QoS0 has no PUBACK.
pending_tracked is not wire inflight. Incomplete tracking cannot establish exact conservation.
Original probe counts deliveries without sequence deduplication; observed loss is not a guaranteed unique-message loss measure.

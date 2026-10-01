# Diagnostic matrix (read raw case JSON for scope)

| Case | Class | Receive/s | Lost | P99 ms | CPU % | RSS MiB | Queue peak | Pending peak | ACK P99 upper us |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| rate-3000-qos-1-observe-1-window-100 | DEGRADED | 2999.70 | 0 | 104.400 | 93.27 | 3.22 | 20 | 64 | 16384 |
| rate-3000-qos-1-observe-1-window-20 | STABLE | 2999.88 | 0 | 38.500 | 88.88 | 3.23 | 25 | 185 | 32768 |
| rate-4000-qos-1-observe-1-window-100 | FAIL | 3104.89 | 51751 | 1635.700 | 77.38 | 5.09 | 52 | 4096 | 1315240.0 |
| rate-4000-qos-1-observe-1-window-20 | FAIL | 3419.81 | 8571 | 8563.600 | 83.77 | 13.92 | 35 | 4096 | 8388610.0 |

ACK histogram quantiles are bucket upper bounds. QoS0 has no PUBACK.
pending_tracked is not wire inflight. Incomplete tracking cannot establish exact conservation.
Original probe counts deliveries without sequence deduplication; observed loss is not a guaranteed unique-message loss measure.

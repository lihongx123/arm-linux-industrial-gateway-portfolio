# Diagnostic matrix (read raw case JSON for scope)

| Case | Class | Receive/s | Lost | P99 ms | CPU % | RSS MiB | Queue peak | Pending peak | ACK P99 upper us |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| rate-3000-qos-1-observe-1-window-20 | STABLE | 2998.91 | 0 | 31.400 | 87.07 | 3.25 | 23 | 189 | 32768 |
| rate-4000-qos-1-observe-1-window-20 | FAIL | 3313.68 | 10963 | 10206.500 | 84.88 | 15.98 | 35 | 4096 | 10304000.0 |

ACK histogram quantiles are bucket upper bounds. QoS0 has no PUBACK.
pending_tracked is not wire inflight. Incomplete tracking cannot establish exact conservation.
Original probe counts deliveries without sequence deduplication; observed loss is not a guaranteed unique-message loss measure.

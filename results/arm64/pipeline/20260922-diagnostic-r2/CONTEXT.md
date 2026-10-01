# Exploratory diagnostic context

The first matrix is exploratory. Native CTest (primarily time-based tests,
observed low process CPU) overlapped the initial low-rate cases and completed
before higher-rate cases. It is not an isolated performance certification.
Repeat the boundary cases alone before making comparative throughput claims.

All cases use the same newly instrumentable ARM64 gateway binary, the same
probe, 2 workers, 1024 queue, 100 simulated device IDs, 2 vCPU and 1 GiB guest RAM.
The observer itself serializes publish/MID registration while enabled, so its
timing impact must be measured; observer-on is not a pure passive baseline.

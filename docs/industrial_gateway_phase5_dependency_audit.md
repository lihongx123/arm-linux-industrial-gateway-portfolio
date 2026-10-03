# Phase 5A dependency and architecture gate (2026-10-02 to 2026-10-03)

The initial gate was performed before Phase 5 production changes. The pre-change Git
state and previous phase evidence are preserved in
`results/edge_core/phase5-20261002/pre-phase5-state.txt`.
OPC UA and S7 were initially dependency-blocked; the table below is their
**final gate status after resolving those dependencies**, not a claim that
they were present in the original Buildroot image.

| Protocol | Gate | Basis and next condition |
| --- | --- | --- |
| OPC UA | READY_FOR_IMPLEMENTATION | `open62541` 1.3.15 (MPL-2.0) was built as a static library for native and Buildroot ARM64. It provides real client Read/Write, subscription APIs, reconnectable sessions, and a local software server. The implementation selects polling rather than claiming subscriptions. ARM64 server/client runtime passed in QEMU. |
| Mitsubishi MC | READY_FOR_IMPLEMENTATION | The Mitsubishi MELSEC communication manual specifies MC 3E binary TCP framing, device codes and `0401`/`1401` batch word read/write. A narrowly scoped D-register, one-word client needs only the existing nonblocking TCP transport. A deterministic socket test server can validate real frames without pretending to be a PLC. |
| Siemens S7 | READY_FOR_IMPLEMENTATION | Official Snap7 client/server source was built as a shared library for native and ARM64, with the LGPL-3.0-or-later header retained and dynamic linkage. A real local TS7Server-backed client integration passed on both platforms. The downloaded `main` archive is identified by SHA-256 below; this is a content-pinned snapshot, not an asserted numbered release. |

The original library checks used `pkg-config`, the dynamic-linker cache,
headers, `dpkg-query`, and the Buildroot configuration/sysroot/package tree.
Neither library was silently vendored or enabled in the existing Buildroot
image. They remain opt-in `WITH_OPCUA` / `WITH_S7` build dependencies and are
injected into a **private QEMU test image**, leaving the historical image and
results untouched.

| Dependency | Source identity | Native build/link | ARM64 build/link |
| --- | --- | --- | --- |
| open62541 | v1.3.15 archive, SHA-256 `1F71C1A57EB98B2CF3C8F92EE9EFCC7E3139932A97546A2BC8DFFCFB7F1B0FD9` | `/tmp/mqmgateway-open62541-native-install/include`, `lib/libopen62541.a` (static) | `/tmp/mqmgateway-open62541-arm64-install/include`, `lib/libopen62541.a` (static) |
| Snap7 | official `main` snapshot archive, SHA-256 `E8CC1164D1A6A1DFB9B644E99915DFB4AB3A13232551DF0E8E6A0F8184134738`; archive source header mentions 1.4.0 while README says repository starts with 1.4.3, so no numbered release is claimed | `release/Wrappers/c-cpp/snap7.h`, `build/bin/linux/libsnap7.so` (shared) | same header, `build/bin/linux/libsnap7-aarch64.so` (shared; injected with this ELF filename) |

The Snap7 snapshot is not checked into this repository; reproducible reuse
requires retaining the verified archive or retrieving content with this exact
hash. Redistribution of its dynamically linked binary must observe LGPL
terms. This implementation is scoped to DB-area word access over S7 TCP; the
Snap7 address model also offers M/I/Q areas, but this driver does **not**
implement them. OPC UA is software-validated without certificate/security
mode coverage. Neither protocol was tested with a physical PLC.

The MC subset selected for this phase is TCP, MC 3E frame, **binary** encoding,
CPU access route (`network=00`, `PC=FF`, `I/O=03FF`, `station=00`), D-area only,
one unsigned 16-bit word per request. Explicit D addresses are decimal and
bounded to 24 bits. No M bits, ASCII 3E, 4E, random access, multi-register
batching, or universal Mitsubishi PLC compatibility is claimed. The local test
server is test-only; production uses the existing TCP transport and reactor.

Architectural fit: one MC driver per configured endpoint/device, explicit
point registration, GatewayCore/PointMapper for telemetry and commands, and
no protocol selection branch in GatewayCore. The inherited TCP driver uses
monotonic response/reconnect timers and a bounded command queue. A successful
socket write is **not** a successful MC command: only a valid end-code-zero
response completes it. A disconnected in-flight write is not replayed.

References: [Mitsubishi MELSEC communication protocol manual](https://dl.mitsubishielectric.com/dl/fa/document/manual/plc/sh080008/sh080008ab.pdf),
[open62541 official source](https://github.com/open62541/open62541),
[Snap7 official source](https://github.com/davenardella/snap7),
[Snap7 distribution/license information](https://sourceforge.net/projects/snap7/).

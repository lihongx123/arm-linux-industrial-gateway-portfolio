# Phase 3 closure audit — 2026-10-02

Stage A result: PASS. Audited the live mqmgateway_iot path before beginning Phase 4. Baseline HEAD 644187153744cf7ecb66ccdca9f9be44ba87e8f5; all Phase 1–3 work remains uncommitted.

## Gap A1: PointRegistry and PointMapper

Before closure, GatewayCore accepted empty point IDs for production telemetry and PointRegistry was only consulted for an optional point. CAN published raw hex, RTU kept a complete wire frame, Modbus TCP and Generic TCP published bytes. No cooked value was generated in the live MQTT path.

After closure, each production driver supplies a point definition, GatewayCore registers/looks it up, PointMapper validates and maps raw bytes, and the V2 message enters the existing bounded queue. MQTT keeps old fields and adds point_id, raw_value, value and driver_id. RTU's point raw bytes are extracted from the f03 response while the original wire frame remains in the legacy payload for compatibility.

| Driver | Input | Device | Point | Point raw | Cooked / metadata |
| --- | --- | --- | --- | --- | --- |
| CAN | SocketCAN frame | can-{frame id} | frame | CAN data bytes | hex, CAN address/protocol |
| Modbus RTU | CRC-validated f03 response | configured rtu-{slave} | holding-{register} | two register bytes | unsigned BE integer, slave/address/protocol |
| Modbus TCP | MBAP-validated f03 response | configured ID | holding-{register} | two register bytes | unsigned BE integer, transaction checked, slave/address/protocol |
| Generic TCP | validated len16be frame | configured ID | payload | frame payload | hex, generic_tcp protocol |

PointMapper supports scale/offset for integer points and refuses malformed width/nonfinite mapping. Dynamic devices and points each have a 65536-entry cap. Status messages also acquire a point identity through the driver definition. Unit checks and live MQTT tests check actual point fields rather than mere interface existence.

## Gap A2: Generic TCP configuration

Old shared TcpConfig carried Modbus slave/register fields into GenericTcpDriver. The common TcpConfig now contains only endpoint, polling/reconnect and timeout fields. ModbusTcpConfig owns slave/register. Generic CLI is id,IPv4,port[,len16be,reconnect_ms,response_ms]. The len16be framing is explicit; Generic TCP never reads Modbus fields.

## Anti-fudge review

GatewayCore has no protocol send branch; Reactor dispatches registered driver events and dynamic FD tokens. Configured RTU/TCP identities are explicitly bound in DeviceRegistry. CAN accepts historical aliases without checking rtu- prefixes; unknown ambiguous identities are rejected by the manager. Modbus TCP is inside mqmgateway_iot, Generic TCP uses a nonblocking socket, and the old Reactor RTU state machine is absent. PointMapper is invoked in GatewayCore.onDriverMessage, on the real production path before the worker/MQTT.

## Debug record

First CTest after point change failed a FakeDriver test because GatewayCore.prepare refused an existing manually registered point. Fixed the fallback to check PointRegistry; the edge-focused test passed.
Second CTest failed one obsolete assertion requiring CAN to reject rtu- IDs. Explicit device binding supersedes prefix exclusion; updated the assertion and added a direct binding/default-command test. Old logs are retained; no assertions were removed to conceal a runtime failure.

## Validation

- Native build PASS; [edge] 9 cases, 103 assertions PASS.
- Final complete CTest 5/5 PASS (see ctest-closure-pass.log).
- Local MQTT point E2E PASS for Modbus TCP and Generic TCP (mqtt-point/summary.json, mqtt-final/summary.json).
- CAN runtime 4/4 PASS (can-final.log).
- Mixed CAN/RTU 10 s: CAN 1000/1000, RTU 102/102, no missing/duplicates; both commands confirmed (mixed-routing/summary.json).
- TCP driver probe PASS; RTU driver probe PASS.
- Phase 3 historical results remain untouched.

Stage A does not claim new hardware or cloud verification.

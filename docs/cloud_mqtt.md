# EMQX Cloud TLS mode (CAN gateway)

`mqmgateway_iot` now has an opt-in cloud mode. Local broker behavior and its
existing `device/...` topics remain the default. Cloud mode uses the existing
gateway data path and libmosquitto client; it does not create a second MQTT
client or change the Modbus service configuration.

## Runtime configuration

Supply credentials through the process environment or a secret manager. Never
put them in the command line, source tree, or result files.

| Variable | Meaning |
| --- | --- |
| `EMQX_HOST` | EMQX broker DNS hostname; used for TLS hostname verification/SNI |
| `EMQX_PORT` | TLS listener, normally `8883` |
| `EMQX_CA` | Path to the trusted CA certificate bundle/file |
| `GATEWAY_MQTT_USERNAME` | Project A broker username |
| `GATEWAY_MQTT_PASSWORD` | Project A broker password |
| `GATEWAY_MQTT_TLS` | Optional `1`/`true` or `0`/`false` for non-cloud TLS mode |

Start with `mqmgateway_iot --cloud`. The mode requires a non-loopback host,
both credential values, and a CA path. Environment settings override ordinary
host/port/TLS command-line settings; cloud mode always requires verified TLS.
No option disables peer verification. The TLS client is configured for TLS
1.2 or newer and uses the broker hostname supplied to `connect_async` for peer
identity checking. Local mode does not require credentials or a certificate.

## Cloud topic mapping

- Uplink: `resume/gateway/devices/<device_id>/telemetry`
- Reserved device-liveness namespace: `resume/gateway/devices/<device_id>/status`
- Downlink subscription: `resume/gateway/devices/+/command`
- Command result: `resume/gateway/devices/<device_id>/command/result`
- Gateway status/heartbeat: `resume/gateway/status/<client_id>[/heartbeat]`

The current SocketCAN path has no per-device presence/heartbeat source, so it
does not publish fabricated device online/offline events. Gateway process status
and heartbeat are published under the gateway status namespace; command
outcomes use the command-result topic.

The downlink payload is passed through the existing CAN command validator
(`can_id`, hexadecimal `data`, optional `timeout_ms`) and then the existing
bounded queue/SocketCAN path. A full downlink PASS still requires observing the
matching simulated CAN output, not merely the MQTT callback.

## Validation status (2026-09-23)

### Real EMQX Cloud Validation

Project A was validated against
`kf7bd96b.ala.dedicated.aliyun.emqxcloud.cn:8883` using the `gateway-client`
account. The password remained only in the private WSL environment file and is
not present in source, documentation, commands saved as evidence, or result
files.

- TLS 1.3 handshake: PASS. The configured CA, peer verification, hostname
  verification, and SNI were enabled; OpenSSL returned verify code 0.
- Independent MQTT authentication and QoS 1 publish/subscribe: PASS.
- Uplink: PASS. `vcan0` input `123#A1B2C3D4` traversed the repository gateway
  and an independent subscriber received one message on
  `resume/gateway/devices/can-291/telemetry` with matching `device_id`, CAN
  address, and payload.
- Downlink and command routing: PASS. An independent publisher sent a command
  for `gateway-demo-001`; the command passed through the existing router and
  produced `456#DEADBEEF` on simulated CAN.
- Command result: PASS. An independent subscriber received the successful
  result from
  `resume/gateway/devices/gateway-demo-001/command/result`.
- Real broker recovery: PASS. The gateway process was reversibly paused for 20
  seconds, exceeding the real broker's keepalive window while an independent
  subscriber stayed live. EMQX expired the connection; after the process was
  resumed, the disconnect callback was observed at 20,016 ms from isolation
  start and reconnection completed 1,113 ms after recovery. Subscription
  recovery, the first telemetry (21 ms), and the first command (209 ms) were
  independently verified. Telemetry injected during isolation arrived after
  recovery, and final publish failures were zero.

Evidence is under `results/cloud-live-emqx/`.

### Real EMQX Sustained Communication Test

On 2026-09-23, the native gateway ran against real EMQX Cloud with TLS,
verified CA and hostname, and MQTT QoS 1. A paced SocketCAN generator sent
100 frames per second for 1,800.000 seconds. Each eight-byte CAN frame carried
a four-byte run identifier and a four-byte big-endian sequence number. The
existing gateway model published these as hexadecimal payloads on
`resume/gateway/devices/can-801/telemetry`. An independent TLS subscriber
parsed the payload and checked every sequence number; raw messages were not
retained in the repository.

| Measure | Real EMQX result |
| --- | ---: |
| Generated and gateway publish attempts | 180,000 |
| Gateway API accepted and QoS 1 PUBACK received | 180,000 |
| Independent subscriber deliveries / unique sequences | 180,000 / 180,000 |
| Missing / duplicates / out of order | 0 / 0 / 64 |
| Publish failures / queue rejects / gateway reconnects | 0 / 0 / 0 |
| Average generated rate, measured with monotonic time | 99.99999 msg/s |
| Low-frequency commands sent / observed on simulated CAN / results received | 30 / 30 / 30 |

Each downlink request carried a unique `command_id` and a distinct four-byte
CAN payload. The current command result schema returns status and detail but
does not echo `command_id`, so command results were counted with one command
outstanding per minute and matched to observed CAN payloads by interval. The
64 out-of-order first deliveries were measured; they did not cause missing or
duplicate deliveries. WSL wall-clock timestamps shifted during the run, so
duration and average rate use a monotonic clock.

The sustained-test evidence is under `results/cloud-soak-emqx/`. These real
cloud communication measurements are separate from the Local/QEMU Performance
measurements in the other results directories.

After the 100 msg/s run passed, a second real cloud run sent 250 messages per
second for 600.000 seconds. It generated 150,000 CAN frames; the gateway
received 150,000 QoS 1 PUBACKs and the independent subscriber received 150,000
unique sequences, with zero missing, duplicate, publish failure, or queue
rejection events. Two first deliveries arrived out of order. This second run
tested sustained uplink only. It does not establish an EMQX plan or gateway
throughput ceiling.

### Local TLS Broker Validation

- Native cloud-config argument and credential-redaction smoke test: PASS.
- Local TLS-broker end-to-end simulation: PASS (temporary CA and DNS `localhost`
  certificate; peer verification; cloud-topic uplink; cloud-topic CAN downlink
  and command result; broker restart and subscription recovery). Startup-to-online
  was 101 ms and broker interruption-to-online was 1076 ms. This is a local
  protocol/TLS test, not an EMQX Cloud measurement.
- Existing native CAN↔MQTT, invalid-command and invalid-CAN integration: PASS.
- Existing broker outage/recovery and overload regression: PASS (reconnect
  1.516 s; queue peak 4; 644 rejected; 226 command timeouts; 0 publish failures).
- Buildroot ARM64 cross-build of the modified application: PASS.
- QEMU ARM64 existing functional suite with the modified binary: PASS; CAN to
  MQTT, MQTT to CAN, invalid-command rejection, broker reconnect, and 500/500
  local performance messages passed in 20.925 host seconds.
- Full native CTest: 2/3 targets pass. The unit target reports 155/158 cases;
  three ExprTk converter cases fail because optional ExprTk was not found at
  configure time (`EXPRTK_INCLUDE_DIR-NOTFOUND`). This limitation predates the
  cloud configuration change and its raw output is retained under
  `results/cloud-live-baseline/` and `results/cloud-live-local/`.

The local measurements above remain local-only baselines and are not used as
the real EMQX timings.

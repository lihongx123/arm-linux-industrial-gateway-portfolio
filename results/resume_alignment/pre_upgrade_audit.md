# Pre-upgrade audit — 2026-09-30

Requested repository name: arm-linux-industrial-gateway

Actual absolute path: C:/Users/hello/Documents/Codex/2026-09-21/files-pasted-by-the-user-br-2/outputs/mqmgateway

HEAD: e91a31762c07aebe50c448964c8a2d5af2ebe52d

Current mqmgateway_iot owns CAN only; RTU termios parser exists as a separate library/test while production RTU remains in upstream modmqttd. No simultaneous RTU/CAN integration in one process. CAN/RTU source, parser tests, gateway queue/worker and MQTT implementation inspected. Keep upstream legacy unchanged; add serial transactions and CAN to one southbound epoll reactor, shared UnifiedMessage/queue/workers. Defaults workers=2 queue=1024 remain. Existing tracked modifications are predominantly line endings; preserve all of them plus untracked interview guide. Historical tests are not validation of new architecture.

Raw status/source fingerprints and historical evidence inventory are saved under `20260930T143425Z/pre-state.json`. New results use this dated directory only. No old evidence is to be overwritten. This is a pre-upgrade audit, not a PASS record; new claims start NOT_IMPLEMENTED until source and tests justify another allowed status.

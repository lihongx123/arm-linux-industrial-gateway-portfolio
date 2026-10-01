# 实施日志

## PHASE 1 — PASS

审计原始目录、构建、对象持有关系、MQTT/Modbus/queue/thread 数据流。仅新增 `docs/original_architecture.md`。

## PHASE 2 — PASS after remediation

原始基线 build PASS，但 CTest 0 tests，直接 Catch2 因 ExprTk 缺失为 146/149。保留失败证据后引入临时官方 ExprTk header、标准 Release build，并只补 CTest 注册；修复后 173/173 原始用例通过。后续新增 3 个 extension test 后最终 CTest 仍通过。

## PHASE 3 — PASS

新增 Mosquitto、publisher/subscriber/roundtrip scripts 和 Modbus TCP data store。真实 Cloud command → broker → `modmqttd` → Modbus write/read → broker → subscriber 为 20/20。

## PHASE 4 — PASS

新增 socat PTY pair 和手写 RTU slave。真实 CRC16 bytes 支持 input/holding/coil/read/write、timeout、bad CRC、invalid frame；8/8 checks。

## PHASE 5–7 — PASS

新增 SocketCAN/epoll、统一消息、CAN/MQTT 双向和 command router。vcan 双向及非法输入检查通过。

## PHASE 8 — PASS

新增 bounded queue、固定 worker 和全套 metrics。容量 4 的故障测试实际达到 peak 4 并拒绝 641 条。

## PHASE 9 — PASS

新增 heartbeat、keepalive、disconnect/reconnect、publish retry/backoff、command deadline、error isolation、graceful shutdown。Broker 恢复与 fault tests 均通过。

## PHASE 10 — PASS

新增四个统一入口、A–J 场景、CPU/RSS/latency/throughput/loss 原始记录。最终 CTest 和所有统一 runner 通过。

## 仍需目标机工作

- 在实际 ARM Linux/真实 CAN/RS485 硬件上重跑全部数据；
- 将 60 秒稳定性扩展到生产要求的小时/天级；
- 5000/s burst 需要 broker/network/worker 调优或明确限流；
- 若要求单进程统一模型，需要把原 `modmqttd` 的 Modbus `QueueItem` 逐步适配到 `UnifiedMessage`，当前采用低侵入双进程共享 broker 架构。

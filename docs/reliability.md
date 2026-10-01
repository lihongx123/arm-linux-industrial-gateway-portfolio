# 可靠性

| 机制 | 实现与触发证据 |
|---|---|
| MQTT keepalive | libmosquitto async connection，默认 10 秒 |
| disconnect detection | disconnect callback 清除 connected 状态 |
| reconnect | libmosquitto exponential reconnect delay 1–30 秒 |
| publish retry | 最多 6 次，100 ms 起、上限 1000 ms 指数退避 |
| heartbeat | 独立 heartbeat thread，含 queue depth/peak |
| command timeout | worker 发送前检查 enqueue deadline |
| backlog protection | bounded queue + reject-new |
| Modbus timeout/retry | 保留原实现；RTU simulator 实测 timeout 检出和恢复 |
| CRC/invalid frame | RTU bad CRC/invalid frame、CAN error/RTR frame 均实测隔离 |
| graceful shutdown | SIGINT/SIGTERM 停止 queue/thread、发布 offline、关闭 MQTT/CAN |

Broker disconnect 场景最终一次统一测试测得恢复 1.539577 秒；恢复期间注入的 CAN telemetry 最终到达，publish final failure 为 0。短时间 burst 的容量边界见 benchmark：5000/s 时出现 39.45% loss，生产配置应限流或扩大下游吞吐能力。

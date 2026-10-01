# 并发与背压

- SocketCAN 接收线程仅做 frame 校验、统一消息转换和入队。
- Mosquitto 网络线程只做 topic/payload 校验和入队。
- 固定数量 worker 执行 CAN transmit 或 MQTT telemetry publish。
- `BoundedQueue` 不会无限增长；满时拒绝新项，状态回报 `queue capacity exceeded`。
- 默认 queue capacity 1024、workers 2，均可通过命令行调整。

指标文件包含 `enqueued`、`dequeued`、`current_depth`、`peak_depth`、`rejected`、`processing_latency_ms`、`published`、`publish_failures`、`command_timeouts`、`can_errors`。

故障实测将容量设为 4、worker 设为 1、处理延迟设为 5 ms，实际 peak 4、reject 641、command timeout 223，证明背压和 deadline 不是只写日志。`--processing-delay-ms` 默认 0，仅用于确定性故障/压力测试。

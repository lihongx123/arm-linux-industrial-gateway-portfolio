# 并发与背压

## Phase 4.5 当前线程/所有权

- 南向 epoll reactor、定时采集线程调用 `GatewayCore`，后者向 Gateway 的有界工作队列推入值类型 `UnifiedMessageV2`；命令结果有 ID 时直接由管理器入原适配器的有界发送队列。
- 固定 worker 从 Gateway 工作队列取出普通上行并调用 `NorthboundManager::publish(const UnifiedMessageV2&)`；管理器顺序调用各适配器的**非阻塞有界入队**接口，适配器复制序列化后的主题/内容，不持有调用者消息引用。
- 每个 MQTT 实例有一条独立发送线程，执行 libmosquitto publish 与最多 6 次有限重试；未完成 publish ID 的数量还受 `mqttMaxInflight` 限制。一个实例断线时只会占满自己的有界队列，不占住 Gateway worker 或另一个适配器的发送线程。
- Mosquitto 网络回调线程解析入站命令，管理器检查 ID/deadline 并只向有界命令工作队列提交；网络回调不执行南向 I/O。manager 的命令表有 4096 上限；锁不跨网络 I/O。
- Gateway 先停止南向接收与 worker，再令管理器对未完成命令生成失败回执，MQTT 适配器在连接可用时最多等待 2 秒排空，随后断开。离线/超时导致的未发送消息计入 `dropped`，不是持久化交付保证。

后文默认队列和早期背压实测为历史阶段记录，仍可用作参数背景，但不能当作新北向实现的压测结论。

- SocketCAN 接收线程仅做 frame 校验、统一消息转换和入队。
- Mosquitto 网络线程只做 topic/payload 校验和入队。
- 固定数量 worker 执行 CAN transmit 或 MQTT telemetry publish。
- `BoundedQueue` 不会无限增长；满时拒绝新项，状态回报 `queue capacity exceeded`。
- 默认 queue capacity 1024、workers 2，均可通过命令行调整。

指标文件包含 `enqueued`、`dequeued`、`current_depth`、`peak_depth`、`rejected`、`processing_latency_ms`、`published`、`publish_failures`、`command_timeouts`、`can_errors`。

故障实测将容量设为 4、worker 设为 1、处理延迟设为 5 ms，实际 peak 4、reject 641、command timeout 223，证明背压和 deadline 不是只写日志。`--processing-delay-ms` 默认 0，仅用于确定性故障/压力测试。

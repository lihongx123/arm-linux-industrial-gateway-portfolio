# Phase 4.5 — 北向核心架构

## 范围与边界

`mqmgateway_iot` 的南向驱动仍由 `GatewayCore`、`DriverManager`、`IDeviceDriver` 管理。北向改为 `NorthboundManager` 持有多个 `INorthboundAdapter`；当前真实适配器为 `MqttNorthboundAdapter`，沿用 libmosquitto、既有认证/TLS 配置和旧主题。上游独立进程 `modmqttd` 不经此管理器，原源码与历史证据未改动。本阶段没有接入任何云厂商 SDK，也没有在新路径重测公共云或真实硬件。

```text
IDeviceDriver → GatewayCore → 有界工作队列 → NorthboundManager
                                             ├─ MqttNorthboundAdapter A → libmosquitto
                                             └─ INorthboundAdapter B    → 可注册的另一实例
```

`GatewayCore` 只处理 `UnifiedMessageV2`、设备/点位和驱动，不含 MQTT 主题或 libmosquitto 类型。Gateway 负责线程、工作队列与北向管理器接线；协议序列化和主题全部在 MQTT 适配器。适配器接口的 `publish` 表示**进入该实例的有界发送队列**，不等于 broker ACK。实际 `mosquitto_publish` 成功、API 失败、队列拒绝分别计数。

## 语义模型

`UnifiedMessageV2::northboundType` 是传输无关枚举：`Telemetry`（频变过程量）、`Attribute`（较稳定配置/元数据）、`Status`（设备或网关运行状态）、`Alarm`（需关注的异常）、`Diagnostic`（内部技术/健康信息）、`Command`（控制请求）、`CommandResult`（关联回执）。消息保留设备/点位、质量、原始/映射值、时间、驱动、`correlationId`、来源适配器、操作、deadline 与状态。旧 `UnifiedMessage` 增加关联 ID，使 RTU 排队及回执的 V1/V2 往返不丢 ID。

运行链路已产生 Telemetry、Status、Diagnostic、Command、CommandResult；Phase 6 后健康/队列告警也沿独立 Alarm 主题产生、确认和恢复。Attribute 仍只有枚举、主题和序列化，没有生产者，不能写成端到端已验证。诊断快照为保持此前 JSON 格式采用显式 `legacy_snapshot` 兼容路径。

## MQTT 主题与兼容性

| 语义 | 本地模式 | Cloud 模式 | 兼容说明 |
| --- | --- | --- | --- |
| 遥测 | `device/{id}/telemetry` | `resume/gateway/devices/{id}/telemetry` | 原主题/字段保留，点位字段仍附加 |
| 设备状态 | `device/{id}/status` | `resume/gateway/devices/{id}/status` | 原主题保留 |
| 入站命令 | `device/{id}/cmd/{operation}` | `resume/gateway/devices/{id}/command` | 继续使用 `CommandRouter` 校验旧命令格式 |
| 命令结果 | `device/{id}/status` | `resume/gateway/devices/{id}/command/result` | 原主题保留；JSON 新增 `command_id`、`state` |
| 网关状态/心跳 | `gateway/{client}/status`、`gateway/{client}/heartbeat` | `resume/gateway/status/{client}`、其 `/heartbeat` | 原主题保留 |
| 诊断 | `gateway/{client}/diagnostics` | `resume/gateway/status/{client}/diagnostics` | 按原 JSON 结构兼容 |
| 属性 | `device/{id}/attribute/{point}` | `resume/gateway/devices/{id}/attribute/{point}` | 新增主题，未接生产者 |
| 告警 | `gateway/{client}/alarm` | `resume/gateway/status/{client}/alarm` | Phase 6 健康/队列事件生产者；本地 MQTT 已验证，云端未复验 |

接口还可表示出站 `Command`，但目前实际下行只通过原入站命令订阅。`CommandResult` **没有另发一个新的本地 `/command_result` 主题**，以免无通知地改变旧订阅者行为；消费者以 JSON 的 `command_id`/`state` 区分命令回执和普通设备状态。

## 命令生命周期

```text
MQTT callback → CommandRouter/元数据解析 → NorthboundManager 去重及 deadline 检查
             → GatewayCore.prepare → 有界工作队列 → worker → GatewayCore.submit
             → DriverManager/driver → 带相同 correlationId 的驱动状态
             → NorthboundManager → 原始入站适配器的有界发送队列 → broker
```

JSON 可含 `command_id`（1–128 字符）、`deadline_ms`（Unix 毫秒绝对截止时间）、`timeout_ms`（毫秒）。缺少 ID 时生成 `adapterId-序号`；缺少绝对 deadline 时使用接收时间加 timeout。管理器的 ID/状态表上限 4096；已完成的最早记录在容量压力下可淘汰，活跃记录不会被淘汰。相同 ID 在保留期间拒绝执行。状态 `accepted` 表示已进入网关工作队列，终态为 `succeeded`、`failed`、`timeout` 或 `rejected`；最终 JSON 的 `state` 表示此生命周期，`status` 尽量保留旧驱动的 `ok`/`success`/错误值。队列满、解析错误、未知驱动与过期请求都有明确拒绝/超时原因。关闭时剩余命令生成失败回执并尝试排空；未连 broker 或超过排空窗口时记录丢弃，不声称远端收到。

`GatewayCore.submit()` 对异步驱动只表示驱动接收命令，成功终态来自驱动后续回执；RTU、TCP 类驱动分别保留关联 ID。队列/管理器 deadline 到期后**不会取消已经交给物理驱动的写操作**；晚到回执被抑制，因此 `timeout` 表示未在期限内确认，不保证设备未执行。Generic TCP 的 `ok` 仅表示帧已发送，不是应用层确认。MQTT QoS1 API 接收和 broker PUBACK 也不能等同于设备操作成功。

## 并发、背压与失败隔离

每个 MQTT 实例有独立发送线程和固定上限队列（默认使用网关 `queueCapacity=1024`）；`publish` 仅复制/序列化后入队，队列满立即拒绝并增加 `dropped`。发送线程复用原有最多 6 次有限重试，并以 publish 回调确认的未完成消息 ID 集合限制向 libmosquitto 提交的并发数量（默认 `mqttMaxInflight=20`）。窗口耗尽只占用该实例的有限重试，不向客户端库无限灌入。一个断线适配器不会让另一个实例等待它的重试。管理器按注册顺序向所有适配器传同一条 `const UnifiedMessageV2&`，适配器不能改动原消息。管理器保留 per-adapter 结果，不因一个适配器失败而停止另一个。

Mosquitto 网络回调线程解析命令后调用统一命令入口，入口只做有界入队。南向 reactor/驱动回调传递命令完成消息到管理器，管理器只入原适配器的发送队列。普通上行先进入 Gateway 有界工作队列，再由 worker 调用 manager。没有在 `GatewayCore` 或管理器全局锁下执行网络 I/O。停止顺序：停止采集与接收、停止/加入工作线程、给未完成命令生成失败结果、有限排空北向队列、断开并销毁 MQTT、停止南向驱动。

指标见网关 `metricsFile` 中 `northbound`：全局及每适配器的发布、失败、丢弃、入站命令、重复、超时、回执、拒绝、健康、队列当前深度与峰值、`pending_publishes`。`command_result_publish_failed` 单独反映回执未能进入目标适配器队列。MQTT 实例的 `published` 计数表示本地 publish API 接收，不等同于远端应用处理。

## 验证状态

- `VERIFIED_SOFTWARE`：8 个北向单元用例（含双适配器 fan-out、失败隔离、语义、生命周期和有界队列）；本地 Mosquitto + 模拟 TCP 设备的 ID 关联、重复/过期/非法命令测试；CAN/RTU 与 TCP 旧主题回归。证据见 [Phase 4.5 目录](../results/edge_core/phase4_5-20261003/)。
- `IMPLEMENTED_NOT_INTEGRATION_VALIDATED`：两个真实 MQTT 实例同时连接的 fan-out；Attribute 生产者；本轮改动后的 ARM64/QEMU、真实 TLS 云 broker。Phase 6 告警本地验证见 [阶段记录](industrial_gateway_phase6.md)。
- `NOT_IMPLEMENTED`：其他北向 SDK/平台适配器、已提交南向写操作的通用取消/撤销保证、离线持久化命令结果。

历史 Phase 5 和此前 ARM64/云端证据仍属于各自当时的源码与测试条件，不能自动继承为本轮二进制的验证结论。

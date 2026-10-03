# MQMGateway 源码学习与当前架构技术文档

> 适用对象：当前 `mqmgateway_iot` 扩展程序。原始升级起点为 `644187153744cf7ecb66ccdca9f9be44ba87e8f5`；本文按 2026-10-04 源码更新。原上游 `modmqttd` 是独立进程，不等同于新驱动框架。测试结论以对应证据目录为准。

## 1. 一句话理解系统

工业设备数据由驱动采集，经 `GatewayCore` 的设备/点位校验与映射，进入有界工作队列，再由北向管理器交给 MQTT 适配器；反向命令从 MQTT 进入，经过 ID、截止时间、设备/点位权限检查后交给对应驱动，回执沿相同关联 ID 返回。

```text
CAN / RTU / TCP / MC / S7 / OPC UA / SPI / I2C / GPIO / ADC / PWM / UART
              │
       IDeviceDriver + DriverManager
              │
 DeviceRegistry + PointRegistry + PointMapper
              │
     GatewayCore / UnifiedMessageV2
              │
       BoundedQueue + worker
              │
        NorthboundManager
              │
      MqttNorthboundAdapter → 本地/远端 broker
```

CAN、RTU、Modbus TCP、Generic TCP、MC 和 Raw UART 的 fd 由 `SouthboundReactor` 用 epoll 分发，协议处理在驱动内；SPI、I2C、GPIO、ADC、PWM、S7、OPC UA 等定时/同步采集进入 `AcquisitionScheduler`，不会硬塞进 epoll。当前共享采集调度器固定 2 个线程。`GatewayCore` 不通过 `if (protocol == ...)` 选择驱动。

## 2. 建议阅读顺序与源码职责

| 顺序 | 文件 | 重点问题 |
| --- | --- | --- |
| 1 | [`src/iot_gateway/main.cpp`](../src/iot_gateway/main.cpp)、[`gateway.hpp`](../src/iot_gateway/gateway.hpp) | 命令行如何变成 `GatewayConfig`；默认容量、线程数、MQTT 参数在哪里 |
| 2 | [`gateway.cpp`](../src/iot_gateway/gateway.cpp) | 启停顺序、工作队列、worker、心跳、指标、命令超时 |
| 3 | [`gateway_core.cpp`](../src/edge_core/gateway_core.cpp) | 注册设备/点位、`prepare()`、`submit()`、`onDriverMessage()`，以及映射失败为什么丢弃 |
| 4 | [`device_driver.hpp`](../src/edge_core/device_driver.hpp)、[`driver_manager.cpp`](../src/edge_core/driver_manager.cpp) | 事件驱动和采集驱动接口、生命周期、命令派发 |
| 5 | [`southbound_reactor.cpp`](../src/iot_gateway/southbound_reactor.cpp)、[`acquisition_scheduler.cpp`](../src/edge_core/acquisition_scheduler.cpp) | fd 就绪与定时采集的两种执行模型 |
| 6 | [`unified_message_v2.hpp`](../src/edge_core/unified_message_v2.hpp)、[`point_mapper.cpp`](../src/edge_core/point_mapper.cpp) | 设备 ID、点位 ID、原始值/工程值、质量、关联 ID、时间语义 |
| 7 | [`northbound_manager.cpp`](../src/northbound/northbound_manager.cpp)、[`mqtt_northbound_adapter.cpp`](../src/northbound/mqtt_northbound_adapter.cpp) | 多适配器扇出、命令去重、主题/JSON、QoS、有限发送队列 |
| 8 | [`diagnostics.cpp`](../src/edge_core/diagnostics.cpp)、[`device_health.cpp`](../src/edge_core/device_health.cpp)、[`alarm_event.cpp`](../src/edge_core/alarm_event.cpp)、[`watchdog.cpp`](../src/edge_core/watchdog.cpp) | 健康状态、告警边沿、确认/恢复、队列停滞；看门狗只观察，不自动重启 |

原始项目与扩展的归属、许可见 [`PROVENANCE.md`](PROVENANCE.md)；不要把原上游代码称作自己从零编写。

## 3. 两条核心调用链

### 上行数据

1. fd 类驱动收到可读事件，或采集调度器到达轮询时间；驱动产生 `UnifiedMessageV2`。
2. `GatewayCore::onDriverMessage()` 确认设备归属和点位，`PointMapper` 对合法数据做类型/比例映射；无效点位被计为 `mapping_failures`，不会直接越过核心发布。
3. `Gateway` 把普通数据放入 `BoundedQueue`；队列满采用拒绝新项而非无界扩容。
4. 固定数量 worker 调用 `NorthboundManager::publish()`；当前真实适配器是 MQTT。MQTT 适配器再使用自己的有界发送队列和独立发送线程；调用 `publish()` 成功只表示**本地入队**，不表示 broker 已确认。

### 下行命令

1. MQTT 回调解析旧 topic/JSON；`NorthboundManager` 生成或读取 `command_id`，检查重复 ID 与 `deadline_ms`/`timeout_ms`。
2. `GatewayCore::prepare()` 检查设备、点位和驱动；命令进入有界工作队列，返回 `accepted` 表示排队成功。
3. worker 在截止时间内调用 `GatewayCore::submit()`；`DriverManager` 找到注册驱动。异步协议的最终成功依赖驱动回执，不能把“提交成功”当成物理写入成功。
4. 回执携带相同 `correlationId`，由北向管理器送回**原入站适配器**。终态为 `succeeded`、`failed`、`timeout` 或 `rejected`。已经发给设备的写操作不具备通用撤销能力，因此超时不等于“设备一定未执行”。

## 4. 参数、默认值与设计理由

| 参数/配置 | 当前默认或范围 | 意义与调整依据 |
| --- | --- | --- |
| `--queue-capacity` | 1024 | Gateway 工作队列上限；背压防止内存随拥塞无限增长。增大只吸收短突发，不会提升慢下游的稳态吞吐。 |
| `--workers` | 2 | 消费工作队列的线程数。过少会增加等待；过多也可能争用 CPU/MQTT 发送窗口。应结合队列峰值与吞吐矩阵调整，不凭空认定越多越好。 |
| `--mqtt-inflight` | 20，允许 1–65535 | 每个 MQTT 适配器未完成发布 ID 的窗口；窗口满会触发有限重试，避免无限提交给客户端库。 |
| MQTT 适配器发送队列 | 默认跟随 `queueCapacity` | 每个实例独立队列和发送线程；一个断线实例不应拖住其他适配器。 |
| `--telemetry-qos` | 1，可设 0 | QoS1 有 broker 确认开销；QoS0 适合可容忍丢失的遥测。两者都不等于业务端已处理。 |
| `--heartbeat-ms` | 1000 ms | 心跳、诊断评估和指标快照周期；缩短会提高观测频度，也增加后台发布/写盘负荷。 |
| `--watchdog-queue-ms` | 5000 ms | 队列非空且出队无进展超过此阈值才报停滞；空队列是空闲，不是故障。 |
| `--health-failures` / `--health-recoveries` | 3 / 2 | 连续失败后 degraded、连续好消息后恢复，形成抗抖动滞回。 |
| `--health-stale-ms` | 0（关闭） | 超时未收到好消息时 offline；被动总线未必定期产生报文，需按设备模型显式开启。 |
| `--diagnostics-mqtt` | 关闭 | 开启诊断快照、告警事件与确认主题；指标文件不依赖这个开关。 |
| `--processing-delay-ms` | 0 | 仅用于确定性背压/故障测试，生产不应故意增加处理延迟。 |

以上为**软件默认值**，不是单板极限。历史 2 vCPU/1 GB、100 个模拟设备、2 workers、1024 队列仅属于对应 ARM64/QEMU 实验条件；要证明新的性能边界，需要用**当前二进制**重跑可比测试。历史压力和稳定性结果见 [`EVIDENCE_INDEX.md`](EVIDENCE_INDEX.md)。

## 5. 协议边界：实现了什么

| 领域 | 当前范围 | 学习重点 |
| --- | --- | --- |
| CAN | Linux SocketCAN 收发，vcan 可做无硬件回归 | 帧 ID、错误/RTR 隔离、fd 生命周期 |
| Modbus RTU | termios/PTY、CRC、读/写、单事务在途 | 串口时序、半包、超时、RS485 传输与协议分层 |
| Modbus TCP / Generic TCP | 独立配置；Generic TCP 使用 `len16be` 长度帧 | TCP 是字节流，不能假设一次 read 就是完整报文 |
| Mitsubishi MC | MC 3E binary、D 区单个 16 位 word 读/写 | 有限协议子集与错误回执，不是完整 MC 协议族 |
| Siemens S7 | Snap7，DB word 读写 | 同步库调用的调度、超时与断线恢复 |
| OPC UA | open62541 客户端定时读/可写点 | 当前是 polling，不是 Subscription；安全模式/证书未完成 |
| SPI/I2C/GPIO | Linux 后端及软件模拟测试 | 驱动按时采集、点位映射；未做实体总线电气验证 |
| ADC | Linux IIO 原始值属性读取，4 字节无符号整数及 scale/offset | 原始码与工程值分层、输入合法性、定时采样 |
| PWM | Linux PWM 属性写入、8 字节占空比命令、读回与停机安全态 | 周期/占空比约束和软件回执与真实波形的区别 |
| Raw UART/RS485 | Raw UART 定界或固定长度帧，RS485 配置能力 | UART/RS485 是传输层，不等于 Modbus RTU |

ADC/PWM 的软件与 ARM64 客体仿真、配置和边界见 [ADC/PWM 技术文档](ADC_PWM_TECHNICAL_GUIDE.md)。S7/OPC UA 依赖可选，基础构建可关闭。其他协议子集和 ARM64 证据分别见 [`industrial_gateway_phase4.md`](industrial_gateway_phase4.md)、[`industrial_gateway_phase5.md`](industrial_gateway_phase5.md)。

## 6. Phase 6 可靠性与可观测性

`DeviceHealth` 使用单调时钟判断 stale，墙钟只用于事件时间戳；状态为 `unknown → online/degraded/offline`，失败和恢复阈值避免单次波动导致告警风暴。`AlarmLedger` 只在状态边沿生成 `raised/updated/acknowledged/recovered`，历史最多保留 256 条；诊断 JSON 的设备明细最多展开 64 条。`QueueWatchdog` 检测真实堆积且无出队进展，**仅报告**，不会杀掉或重启进程。启用 `--diagnostics-mqtt=1` 后，诊断快照沿旧 `gateway/{client}/diagnostics`，告警事件沿 `gateway/{client}/alarm`；确认请求沿诊断 `/ack`，回执沿 `/ack/result`。事件发布为内存队列传递，无离线持久化保证；`alarm_history_missed` 可发现事件历史超限造成的缺口。

## 7. 验证与结项边界

- Phase 1–5 的逐阶段记录见 [`industrial_gateway_upgrade_plan.md`](industrial_gateway_upgrade_plan.md) 及对应阶段文档。
- Phase 4.5 当前代码的本机全量 CTest 为 **11/11 PASS**，北向单元测试 **8 用例/102 断言 PASS**；原始失败和修正后日志见 [`phase4_5-20261003`](../results/edge_core/phase4_5-20261003/README.md)。
- Phase 6 当前本地闭环：[阶段文档](industrial_gateway_phase6.md)与[新证据](../results/edge_core/phase6-closure-20261003/README.md)。相关单元测试 **12 用例/155 断言 PASS**，完整 CTest **11/11 PASS**；独立告警主题、设备 stale/offline/恢复、队列停滞/确认/恢复均以本地 broker/vcan 验证。
- 历史 ARM64 Buildroot/QEMU、8 小时 soak、30 分钟压力及 EMQX 实验**对应各自当时二进制**。本轮 Phase 6 按用户要求只做本地模拟回归，不将历史结果自动写成当前版本的 ARM/云端通过。
- 复现当前功能应使用包含 Phase 1–6.5 与 ADC/PWM 代码的源码快照；每轮历史结果仍对应其当时二进制。

## 8. 学习时最值得自己回答的 6 个问题

1. 为什么 TCP 驱动进 epoll，而 SPI/I2C 采集由 scheduler 驱动？
2. `GatewayCore::prepare()` 和 `submit()` 为什么分两步？异步驱动何时才算命令成功？
3. 两层有界队列分别保护哪一段？队列变大为什么不能消除长期背压？
4. QoS1 PUBACK、网关 `accepted`、驱动 `succeeded` 三个确认分别代表什么？
5. 为什么健康超时用单调时钟、事件记录用墙钟？被动 CAN 设备为什么默认不做 stale 判定？
6. 如何区分队列停滞、broker 断线、设备离线和点位映射失败？各自对应哪些指标和日志？

## 9. Phase 6.5：上报策略与控制面隔离

当前源码先由 GatewayCore 校验/映射，并调用 Diagnostics.observe；Gateway 回调才执行 TelemetryPolicy。COV 表示“值发生有意义变化才上报”，数字死区相对上次成功进入遥测队列的值计算，质量变化无条件上报；maxReportInterval 用 steady_clock 强制周期上报。未配置策略时每个有效样本仍尝试入队。PointMapper 只负责值变换，不保留上报历史。策略细节和例子见 [Phase 6.5](industrial_gateway_phase6_5.md)。

Gateway 现在有独立的 telemetryQueue/commandQueue 和对应 worker；前者满不会占用后者容量。MQTT 适配器还有第二层有界出站队列，解决网络发送背压，不代替 Gateway 的命令执行隔离。区分“命令已进入队列”“Mosquitto publish API 接受”“broker PUBACK”“设备操作成功”，不能把其中一个写成另一个。两个队列和出站队列的限额与拒绝指标都要一起看。

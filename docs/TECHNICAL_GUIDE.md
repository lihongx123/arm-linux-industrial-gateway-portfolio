# 网关参数与技术学习手册

更新：2026-10-01。对应当前工作树；架构升级证据见 [验收记录](resume_architecture.md)。

## 1. 两套入口与模块职责

上游 `modmqttd` 使用 YAML、每网络 Modbus 线程及转换插件。扩展 `mqmgateway_iot` 使用共享南向 Reactor、统一消息和工作队列。阅读时先区分入口，避免把两套线程模型混在一起。

阅读顺序：`src/iot_gateway/main.cpp` → `gateway.hpp/cpp` → `southbound_reactor.hpp/cpp` → `src/serial/` → 队列、路由及集成测试。

南向接收线程持有 CAN/串口事件处理。串口事务在 Reactor 中串行管理；工作线程提交命令，通过 eventfd 唤醒 Reactor，避免多线程同时读写同一 RTU 事务。CAN/RTU 数据转为统一消息，进入同一有界工作队列。

## 2. 配置参数

源码入口：`GatewayConfig`、`RtuConfig` 及 main 参数解析。

| 参数 | 默认值 / 配置入口 | 意义与调整影响 |
| --- | --- | --- |
| MQTT 地址/端口 | 127.0.0.1 / 18883；--mqtt-host、--mqtt-port | 本地 broker；与系统默认 1883 区分 |
| keepalive | 10 s | 保活检测；缩短增加保活频率，故障检测还受网络循环影响 |
| clientId | mqmgateway-iot；--client-id | MQTT 会话身份，多实例应使用不同值 |
| CAN 接口 | vcan0；--can-interface | 本地虚拟 CAN 输入 |
| queueCapacity | 1024；--queue-capacity | 缓冲突发、限制积压；扩大可增加排队尾延迟 |
| workers | 2；--workers | 消息处理并发；增加前先测队列等待和 MQTT 发布阶段 |
| heartbeatInterval | 1000 ms；--heartbeat-ms | 网关状态刷新周期 |
| processingDelay | 0 ms；--processing-delay-ms | 人工处理延迟，用于制造积压 |
| telemetryQos | 1；--telemetry-qos | QoS 1 有确认与重发开销，端到端业务去重需单独考虑 |
| mqttMaxInflight | 20；--mqtt-inflight | QoS 发布在途窗口；过小可限制吞吐，过大增加积压 |
| pipelineMetrics | false；--pipeline-metrics=1 | 分段耗时与发布跟踪，定位排队/处理/确认 |
| RTU 启用 | --rtu-device 指定串口 | 串口资源在启动时打开 |
| RTU 波特率 | 115200；--rtu-baud | 须与设备匹配，PTY 实验关注协议处理 |
| 从站/寄存器 | 1 / 0；--rtu-slave、--rtu-register | 周期读取目标 |
| 轮询/响应超时 | 100 / 1000 ms；--rtu-poll-ms、--rtu-response-ms | 请求节奏和失败判定；超时需结合从站响应和线路时间 |
| 单次 CAN 预算 | 64 帧 | 避免持续 CAN 输入饿死串口 |
| 单次串口预算 | 16 × 512 B | 限制一个事件轮次的处理时间 |
| RTU 解析缓存 | 1024 B | 增量组帧并限制异常输入占用 |

默认线程约为主线程、接收线程、2 workers、心跳线程和 Mosquitto 网络线程，共 6 个。worker 数不是全部线程数。

## 3. 上下行实例

本地主题：`device/{id}/telemetry`、`device/{id}/status`。

- `device/rtu-1/cmd/modbus_write`：`{"slave":1,"register":7,"value":1234}`。
- `device/can-1792/cmd/can_tx`：`{"can_id":1792,"data":"deadbeef"}`。

RTU 写寄存器需要校验回显后回报成功。MQTT 入队、broker 确认与设备执行成功是不同阶段，分析时分别查看计数。

## 4. 测试参数与数据的含义

100 个模拟设备是负载生成器配置；2 vCPU/1 GB 是 QEMU 资源；它们不是设备协议的容量上限。Release 的当前 GCC 构建采用 -O3 -DNDEBUG；-O0/-O1/-O2/-O3 是编译优化等级，不能靠优化等级消除排队、锁竞争或网络确认瓶颈。

当前混合正常流量 30 秒：CAN 3000、RTU 293 全部收到；P99 1.139 ms，网关 CPU 3.076%（单核100%口径），RSS 5956 KiB。最终故障回归 CAN 3000、RTU 273 全收，CRC 拒绝和超时是注入故障的预期观测。

旧 ARM64 500 msg/s 八小时、1500 msg/s 三十分钟，以及后续短阶梯结果保留在历史证据中。比较时固定二进制、模型、QoS、窗口、负载和测量口径，再判断优化收益；当前架构说明列出每轮版本关系。

## 5. 实际问题与解决思路

| 问题 | 处理及学习点 |
| --- | --- |
| CAN/RTU 接入职责割裂 | 统一 Reactor 与消息入口，串口状态集中管理 |
| 忙源占用接收循环 | 每轮读预算控制公平性 |
| 串口 HUP、启动中途异常 | 清理已启动资源，异常退出而非空转 |
| 超时重置误清累计统计 | 清缓冲和清统计分开，保留故障计数 |
| 队列拒绝混入 CAN 错误 | 指标按失败阶段分类 |
| CAN 扩展标识处理 | 按 SocketCAN 标志与标识语义编码 |
| 测试 broker 未启动导致连接失败 | 补齐基础设施，再复测；保留原失败证据 |

若继续设计：先看各段耗时和在途量。队列慢再研究批处理/分流；发布确认慢再评估窗口与 QoS；多串口扩展需要每端点事务状态和公平调度。不能仅增加 workers 就假定线性扩展。

## 如何学习与调整

先沿一条消息读完整链路，再改变一个参数，记录吞吐、延迟、内存和错误数。源码默认值、测试输入和测量结果是三类不同数据；增大队列不等于提高处理能力。本文的调整建议是设计分析，不代表已实施改动。

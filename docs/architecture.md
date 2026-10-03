# 扩展后架构

> 当前 Phase 4.5 北向架构见 [northbound_architecture.md](northbound_architecture.md)。下文记录早期 CAN/Modbus 扩展阶段的设计，不代表当前 `mqmgateway_iot` 的完整组件清单。

当前 `mqmgateway_iot` 的实际路径为南向 `IDeviceDriver`/`DriverManager` → `GatewayCore`/`UnifiedMessageV2` → 有界工作队列 → `NorthboundManager` → `INorthboundAdapter` → `MqttNorthboundAdapter`。CAN、RTU、Modbus TCP、Generic TCP 及后续板级/PLC 驱动共用南向框架。上游 `modmqttd` 仍为独立进程及历史实现。MQTT 主题、连接和序列化已从新网关的 `Gateway` 移至 MQTT 适配器；Gateway 保留线程编排、队列和配置接线。当前只有 MQTT 北向适配器有真实实现。

## 定位

本工程保留上游 `modmqttd` 的 Modbus RTU/TCP、YAML、converter、双向队列、`ModbusThread` 与 Mosquitto 实现，并新增一个独立的 `mqmgateway_iot` 进程承载 SocketCAN、统一消息、路由、有界队列和 worker pool。这样避免重写稳定的 Modbus 核心，同时可让两个进程连接同一 MQTT broker。

## 组件

```text
Modbus RTU/TCP ── 原 modmqttd ───────────────┐
                                             ├── Mosquitto ── Cloud
SocketCAN ─ epoll ─ UnifiedMessage ─ Queue ──┘
                    ▲                 │
                    └── CommandRouter ┴── fixed worker pool
```

- `modmqttd`：原始 Modbus gateway，继续负责配置、轮询、converter 和 Modbus MQTT command。
- `CanSocket`：PF_CAN raw socket、接口绑定、epoll 接收、CAN 发送、error/RTR frame 隔离。
- `UnifiedMessage`：统一描述 timestamp、device ID、protocol、direction、data type、payload、quality 和 address。
- `CommandRouter`：匹配 `device/{id}/cmd/can_tx`，校验 JSON、CAN ID、hex data 和 timeout。
- `BoundedQueue`：固定容量、溢出拒绝，并维护 enqueue/dequeue/depth/peak/rejected/latency 指标。
- `Gateway`：Mosquitto 回调、CAN I/O、固定 worker、heartbeat、publish retry 和 metrics 输出。

## 边界

当前统一消息运行时用于新增 CAN 路径；原 Modbus 路径仍在上游 `modmqttd` 内使用其既有 `MsgRegisterValues`/`QueueItem`。二者在 MQTT topic 与文档化数据语义处汇合。已有 x86_64 WSL2 原生测试和 ARM64 Buildroot/QEMU 软件验证，均未进行真实硬件测试。Buildroot 构建 Linux 系统，QEMU 执行该系统，vcan/PTY 和模拟程序提供设备输入。

新增诊断能力与指标口径见 [分段观测](pipeline_observability.md)。遥测与命令当前仍共用工作队列；分开统计等待时间不等于已经实现队列隔离。

# 工业边缘网关架构升级计划

状态：架构审计、实施规划及阶段记录；下列未来驱动尚未实现。审计基线：`644187153744cf7ecb66ccdca9f9be44ba87e8f5`。`docs/original_architecture.md` 与历史 `results/` 保持不变。

## 1. 当前架构审计

仓库包含两条需要区分的代码路径：原有 `modmqttd` / `libmodmqttsrv` 及其 Modbus/MQTT 单元测试；面向 CAN + Modbus RTU 的扩展可执行程序 `mqmgateway_iot`，位于 `src/iot_gateway/`，串口实现位于 `src/serial/`。本文升级对象是后者；前者不能因为已有 Modbus 功能就视为已接入新驱动框架。

当前扩展程序的数据流如下：

```text
SocketCAN fd ─────────┐
                      ├─ SouthboundReactor (epoll + eventfd + 定时轮询)
RTU termios fd ───────┘      │
                             └─ UnifiedMessage → BoundedQueue → workers → MQTT QoS 0/1
MQTT command → CommandRouter → BoundedQueue → worker → CAN send / RTU submit
                                                RTU submit → reactor → 响应确认 → status
```

实际模块与边界：

| 模块 | 当前职责 | 架构局限 |
| --- | --- | --- |
| `src/iot_gateway/gateway.cpp/.hpp` | 生命周期、MQTT TLS/连接与发布、命令入队、工作线程、心跳及指标 | 同时承担业务编排和协议路由；`workerLoop()` 显式判断 `Protocol::modbus_rtu`，否则走 CAN；云端命令用 `rtu-` 设备 ID 前缀推断协议 |
| `src/iot_gateway/southbound_reactor.cpp/.hpp` | 一个 epoll 实例监听 CAN、RTU 和 eventfd；轮询 RTU、处理超时与确认 | Reactor 直接拥有 `CanSocket` 和 RTU 传输及协议状态；新增 fd 驱动需修改它；目前一条 RTU 事务在途 |
| `src/iot_gateway/can_socket.cpp/.hpp` | SocketCAN 收发并形成统一消息 | 协议实现尚无通用驱动接口 |
| `src/serial/termios_rtu_transport.cpp/.hpp` | 串口读写、RTU 帧解析/CRC | 传输与 RTU 帧解析相邻，尚无 Raw UART / RS485 / RTU 分层接口 |
| `src/iot_gateway/command_router.cpp/.hpp` | 解析 MQTT topic/JSON，验证 `can_tx`、`modbus_write` | 命令集合写死，点位和设备的身份由 topic/ID 约定隐含表达 |
| `src/iot_gateway/unified_message.*`、`bounded_queue.hpp` | 消息结构、序列化与有界队列 | 消息以字节 payload、协议枚举、地址/从站号为主，缺点位语义、关联 ID、来源时间/质量细分；枚举中有 `modbus_tcp` 不等于存在相应驱动 |
| `src/iot_gateway/pipeline_metrics.hpp` | MQTT 发布链路与队列可观测性 | 尚非按设备/驱动统一的健康与诊断模型 |

现有 `unittests/iot_gateway_tests.cpp` 覆盖 CAN 消息序列化、命令路由和队列溢出；`src/iot_gateway/CMakeLists.txt` 注册云配置 smoke；`unittests/` 中大量其他用例针对原有 `libmodmqttsrv`。`tests/load/`、`scripts/`、`buildroot/`、`docs/` 与 `results/arm64/` 提供已有主机及 ARM/QEMU 验证入口和历史证据。既有 CAN、RTU、共享 epoll、队列/worker、MQTT TLS/QoS1、EMQX 与 ARM/QEMU 结果是后续回归基准，不能用“新架构”替代既有功能证据。

## 2. 目标边界与缺失模块

```text
MQTT northbound adapter ── Gateway Core (编排、背压、生命周期)
                              │
                         DeviceRegistry ── PointRegistry
                              │                 │
                         DriverManager ── IDeviceDriver / PointMapper
                              │
                  ┌───────────┴───────────┐
            fd 驱动 / Reactor       采集驱动 / Scheduler
             CAN、RTU、TCP          SPI、I2C、GPIO、ADC、PWM
```

`DeviceRegistry` 管理稳定设备 ID、驱动实例绑定、配置与生命周期；`PointRegistry` 管理设备内稳定点位 ID、类型、单位、读写权限、地址映射和校验；`DriverManager` 管理驱动注册、创建、启动/停止、派发、FD/定时器订阅和能力声明；`IDeviceDriver` 约定命令、事件、采集结果、错误与指标接口。核心只按设备/点位注册信息和能力派发，不出现 `if (protocol == CAN/Modbus/SPI/...)` 分支。

`UnifiedMessageV2` 是拟增加的内部契约：版本、device ID、point ID、消息/关联 ID、方向/类型、类型化值或有界原始字节、质量、来源时间与接收单调时间、命令截止时间、来源/驱动标识。需定义 V1↔V2 适配器，先维持现有 MQTT topic/JSON、QoS、状态和错误语义，另行协商对外 schema 变更。`Device Health` 管理 online/degraded/offline 与过期判定；`Diagnostics` 提供每驱动错误/超时/队列/重连计数；`Alarm/Event model` 提供边沿触发、级别、确认/恢复和去重；`TimeService` 分开墙钟时间戳与单调超时；`Watchdog` 检测采集/工作线程停滞并记录原因，不以重启掩盖产品缺陷。

驱动分类必须保持明确：

| 类别 | 成员 | 运行模型 |
| --- | --- | --- |
| 工业通信/协议 | CAN、Modbus RTU、Modbus TCP、Generic TCP、Siemens S7、Mitsubishi MC、OPC UA | 能提供 fd 的传输经 epoll→驱动回调；协议状态机留在驱动。OPC UA 若采用自带事件循环的库，须通过受控适配器集成，不在核心硬编码 |
| 板级接口/采集 | SPI、I2C、GPIO、ADC、PWM | 调度器/定时器→硬件读写→PointMapper；阻塞调用隔离在受控执行上下文。SPI/I2C 是采集驱动，不设计“SPI Parser”或“I2C Parser”；PWM 属受控输出 |
| 串行层 | UART、RS485、Modbus RTU over RS485 | `SerialTransport`→Raw UART / RS485 传输能力→Modbus RTU 协议驱动；UART 不等于 Modbus，RS485 方向控制与 RTU 帧/时序分别负责 |

## 3. 迁移原则与风险控制

1. 先建立抽象和 V1/V2 兼容适配，再迁移 CAN/RTU，之后才增加协议。每阶段均保持可编译、可回退的完整切片，不一次性替换数据路径。
2. Reactor 只负责 fd 注册、就绪分发、公平预算和唤醒；驱动持有自己的协议状态。RTU 保持单所有者、单事务在途、超时/CRC/静默间隔行为；CAN 与串口仍共享受控事件循环。采集调度独立于 epoll。
3. `DeviceRegistry` 统一解析命令归属，淘汰以 `rtu-` 前缀推断协议；迁移期对旧 ID/topic 保持兼容。点位 ID 与设备地址分离，配置校验阻止重复 ID、非法地址和未经授权写入。
4. 背压始终有界：保留 `BoundedQueue`、worker 数与 MQTT inflight 控制；区分命令与遥测的拒绝、超时及 QoS1 PUBACK，不把提交成功写成云端已收到。扩展驱动时测量队列峰值、公平性、CPU/RSS 与端到端延迟。
5. 历史基准不重写。各阶段新结果写入新的结果目录并附参数、commit、日志、通过/失败判据；不得覆盖 `results/` 既有材料。`docs/original_architecture.md` 保持原样。生产代码更改需遵守上游许可证与署名。

主要迁移风险：V2 消息改变 topic/JSON 导致现有订阅方不兼容；驱动 FD 生命周期与 epoll 中 stale fd/重连竞态；RTU 时序和 CRC 回归；驱动回调阻塞 Reactor 导致 CAN/RTU 饥饿；多个协议共用队列时命令被遥测挤压；设备/点位迁移造成历史指标不连续；可选 PLC/OPC UA 库的版本、许可与交叉编译限制；GPIO/ADC/PWM 与 RS485 方向控制在 QEMU 中无法等同真实板级验证。对这些风险分别设置兼容、故障注入、公平性与平台分层测试。

## 4. 分阶段实施与验收

以下路径中 `src/edge_core/`、`src/drivers/`、`src/board/`、`src/transport/` 和新增测试文件均为**拟建**，当前并不存在。所有阶段共同禁止修改 `docs/original_architecture.md` 与历史 `results/**`；除非单独获批，不触碰原有 `modmqttd/`、`libmodmqttsrv/` 业务逻辑。每阶段遵守“报错→定位根因→最小修正→重编译→受影响测试”的调试闭环；外部依赖缺失、必须依赖真实硬件或架构矛盾时如实停下并记录。

### Phase 1：核心抽象

- 修改/新增：`src/edge_core/{device_registry,point_registry,driver_manager,device_driver,point_mapper,unified_message_v2,time_service}.{hpp,cpp}`（按职责拆分）；`src/iot_gateway/{gateway,unified_message,command_router}.{hpp,cpp}` 增加 V1↔V2 与旧 topic 兼容入口；`src/iot_gateway/CMakeLists.txt`、顶层 `CMakeLists.txt` 接入新库。
- 不修改：`src/iot_gateway/{can_socket,southbound_reactor}.*`、`src/serial/termios_rtu_transport.*`、原有程序及历史资料。
- 测试：新增 registry 重名/缺失/重载、点位类型/权限、驱动生命周期和 V1↔V2 round-trip 单元测试；现有 `iot_gateway_tests`、云配置 smoke、原有 CTest；编译 ARM64 Buildroot 目标并运行短 QEMU 冒烟。
- 验收：当前 CAN/RTU/MQTT 行为和外部 JSON/topic 不变；核心可以仅通过注册表定位设备与点位；无协议专属分支进入新核心接口；队列仍有界。

### Phase 2：迁移 CAN 与 Modbus RTU

- 修改/新增：`src/drivers/{can_driver,modbus_rtu_driver}.*`、`src/transport/serial_transport.*`、`src/edge_core/driver_manager.*`；调整 `src/iot_gateway/{gateway,southbound_reactor,can_socket,command_router}.*` 与 `src/serial/termios_rtu_transport.*` 的适配层和构建定义。逐步让 Reactor 接收驱动 FD/callback，而不直接认识 CAN/RTU 类型。
- 不修改：现有 MQTT topic/JSON 契约、`modmqttd/`、`libmodmqttsrv/`、历史资料；不顺带新增协议。
- 测试：vcan 收发及非法帧、RTU CRC/粘拆包/超时/异常响应、命令回执、FD 断开/重连、公平预算、队列满/停机；现有 CTest、ARM64/QEMU 混合 CAN+RTU 短回归，与基线逐项比对。
- 验收：CAN/RTU 功能、单 RTU 在途、原超时与状态语义不退化；生产核心不按协议分支；驱动停止后无 FD 泄漏、悬挂回调或队列无界增长。

### Phase 3：Modbus TCP 与 Generic TCP

- 修改/新增：`src/transport/tcp_transport.*`、`src/drivers/{modbus_tcp_driver,generic_tcp_driver}.*`、`src/edge_core/{device_registry,point_registry,driver_manager}.*` 的配置与注册、对应 CMake；如需新 CLI/config 项，修改 `src/iot_gateway/main.cpp`。Generic TCP 必须有明确的帧边界策略，不把任意 TCP 字节流当成设备协议。
- 不修改：已稳定的 CAN/RTU 帧处理、原有 `libmodmqttsrv` Modbus TCP 实现及历史证据；仅做适配/复用评估，不宣称原有实现已自动纳入。
- 测试：本地 Modbus TCP 从站/通用 TCP 模拟器、半包粘包、连接断开/重连、请求关联、超时与多设备并发；全量现有测试和 ARM64/QEMU 多驱动短回归。
- 验收：独立配置多 TCP 设备、互不串包；断线恢复不阻塞 Reactor，消息与点位映射正确；原驱动指标不退化。

### Phase 4：板级采集与串行传输

- 修改/新增：`src/board/{spi_driver,i2c_driver,gpio_driver,adc_driver,pwm_driver}.*`、`src/transport/{serial_transport,uart_transport,rs485_transport}.*`、采集 `scheduler`/`point_mapper` 与驱动注册/CMake；RTU 改为可挂接 RS485 传输。具体设备节点、ioctl 和方向脚策略通过平台适配接口注入。
- 不修改：核心业务中的协议分支、MQTT 对外契约、稳定 CAN/TCP 驱动及历史资料；不引入 SPI/I2C parser。
- 测试：mock 总线的定时读取/写入、采集抖动/超时/无设备、点位标定、GPIO 边沿与 PWM 安全默认值；pty 测 Raw UART、RS485 方向切换和 RTU over RS485；Linux/QEMU 能支持的接口单独验证，真实电气层测试另列待办。
- 验收：SPI/I2C/GPIO/ADC 通过调度器触发而非强塞入 epoll；PWM 写入受权限和安全范围约束；Raw UART、RS485 与 RTU 协议边界独立；无硬件时只标记模拟通过，不声称板级实测。

### Phase 5：PLC 与 OPC UA

- 修改/新增：分别新增 `src/drivers/{s7_driver,mc_driver,opcua_driver}.*` 与各自可选依赖/CMake 开关、配置/点位映射和驱动注册；需要专用网络传输时仅扩展相应 transport adapter。
- 不修改：Gateway Core 的协议判断逻辑、已稳定驱动与历史资料；不把未安装库的构建失败伪装为通过。
- 测试：每个协议独立的合法/非法报文、读写、断线恢复、超时/重连、边界值和多设备模拟测试；依赖与许可核查、ARM64 交叉编译、回归测试。OPC UA 证书/安全模式单独验证。
- 验收：每个驱动可独立启停和按需编译，未启用时不影响基础构建；设备/点位契约一致；外部依赖不可用时明确记录该驱动未验证。

### Phase 6：健康、告警与诊断

- 修改/新增：`src/edge_core/{device_health,diagnostics,alarm_event,time_service,watchdog}.*`，扩展 `device_registry`、`driver_manager` 与 `src/iot_gateway/{gateway,pipeline_metrics}.*` 的观测接口；新增独立的健康/事件对外适配，保留旧状态主题。
- 不修改：各驱动的协议编码/帧解析、旧 topic/JSON 及历史资料；告警逻辑不得改变已验证的业务数据路径。
- 测试：offline/degraded/recovered 状态转换、告警去重/确认/恢复、时钟跳变与单调超时、假死/队列停滞、指标基数与有界内存；全量 CTest、ARM64/QEMU 故障注入及短稳定性回归。
- 验收：能定位每个设备/驱动的故障、时间和关联事件；看门狗只报告或按明确策略处置；故障恢复后无重复告警风暴，正常吞吐与延迟不明显退化。

## 5. 推荐实施顺序与验证门槛

严格按 Phase 1→2→3→4→5→6 推进；Phase 5 的三种工业协议可各自独立交付，不要求同一批完成。每个阶段完成后保存新测试参数、原始日志和机器可读摘要，再对照当前 CTest、CAN/RTU、MQTT TLS/QoS1、本地 EMQX 适配与 ARM64/QEMU 基准。真实板级 I/O 和需要特定授权软件的协议只在条件具备时单独验收，不能用 QEMU 结果代替。若回归失败，先判定测试基础设施还是产品缺陷，不为过关而修改历史证据或放宽验收条件。

## 6. Phase 1 实施检查点（2026-10-02，历史记录）

已新增 `src/edge_core/`：`GatewayCore`、`DeviceRegistry`、`PointRegistry`、`IDeviceDriver`（事件驱动与采集能力分开）、`DriverManager` 及 V1↔V2 无损适配，并纳入 CMake；新增 `unittests/edge_core_tests.cpp`。新 `GatewayCore` 已能按注册设备/点位校验并派发命令、接收驱动消息，不包含协议分支。目前这是一层可编译、可测试的**基础契约**，现有网关仍运行原 CAN/RTU 路径；驱动真实迁移、旧核心去协议分支、设备/点位配置加载和采集调度尚未完成，不能把 Phase 1 或整个升级称为已验收。

验证：新的 Release/Ninja 构建成功；最终 `[edge]` 为 4 个用例、58 项断言全部通过；原有 3 个 IoT 基础用例和 `rtu_transport_stream_tests`、`iot_cloud_config_smoke` 通过。完整 CTest（新增第四用例前运行）为 2/3 测试项通过，`unit_tests` 内 161 个用例中 158 通过、3 失败。失败用例是 `Modbus exprtk configuration`、`Expression on list should evaluate`、`Expression on unnamed scalar should be evaluated`；配置显示 `EXPRTK_INCLUDE_DIR-NOTFOUND`，日志原因为找不到 `exprconv.so`，属于现有可选依赖/测试门控问题，本阶段未改动相关业务源码或伪造通过。构建目录 `/tmp/mqmgateway-edge-phase1-build` 中保留完整 `Testing/Temporary/LastTest.log`。本机未生成 `format-check`、`tidy-naming` 目标（缺少相应 clang 工具），未执行自动格式化。

## 7. Phase 2 实施状态（2026-10-02）

CAN 与 Modbus RTU 已迁移到 Driver Framework，真实上下行及驱动状态经过 GatewayCore；Gateway 无协议派发分支，旧 Reactor 的 RTU 状态机已移至唯一 RTU 驱动。逐协议、混合故障、本地 TLS、完整原有 CTest 及 ARM64/QEMU 验证通过。详见 [Phase 2 实施与证据](industrial_gateway_phase2.md)。第 6 节保留 Phase 1 当时的结果；配置现有 ExprTk 依赖后原有完整 CTest 已通过。

## 8. Phase 3 实施状态（2026-10-02）

Modbus TCP 和 Generic TCP 已作为独立设备驱动接入 GatewayCore、动态 FD Reactor 与现有 MQTT 链路。详细实现、配置格式、逐协议验收和限制见 [Phase 3 实施与证据](industrial_gateway_phase3.md)。Phase 4–6 尚未实施。

## 9. Phase 4 实施状态（2026-10-02）

上段是 Phase 3 完成时的记录；随后 Stage A 已关闭点位映射和 Generic TCP 配置耦合缺口，Phase 4 已实现 SPI/I2C/GPIO 的 Linux 生产后端、Raw UART/RS485 串口模式及有界共享采集调度。软件/ARM64 QEMU 验证和硬件边界见 [Phase 4 实施与证据](industrial_gateway_phase4.md)。ADC/PWM 仍为 NOT_IMPLEMENTED_DEFERRED；Phase 5–6 未实施。

## 10. Phase 5 实施状态（2026-10-03）

第 9 节是 Phase 4 结束时的历史状态。随后 MC 3E binary/TCP D-word、open62541 OPC UA polling 客户端及 Snap7 S7 DB-word 客户端已接入同一 GatewayCore，并通过本机完整 CTest 11/11 和 ARM64/QEMU 11 设备混合软件验证。各协议的精确边界、可选依赖、失败历史与证据见 [Phase 5 实施与证据](industrial_gateway_phase5.md)。ADC/PWM 与 Phase 6 仍未实施。

## 11. Phase 6 实施状态（2026-10-03）

上一节是 Phase 5 结束时的历史状态。当前未提交工作区已加入设备健康、有限告警历史与确认、诊断快照、单调时钟 stale 判定和只报告不重启的工作队列看门狗；独立告警 MQTT 主题已用本地 Mosquitto/vcan 验证。完成范围、参数、当前本地证据和未验证边界见 [Phase 6 实施与证据](industrial_gateway_phase6.md)。ADC/PWM 仍未实现；本轮没有用历史 ARM64/云端结果冒充当前源码的复验。

## 12. Phase 6.5 上报策略与队列隔离

Phase 6 之后增加协议中立的 COV/数字死区/最长上报间隔，并将 Gateway 共享工作队列拆为有界遥测与命令队列、独立 worker 和看门狗。MQTT 适配器也在原总容量内保留控制消息槽位。实现、配置、测试和限制见 [Phase 6.5 记录](industrial_gateway_phase6_5.md)。本轮仅做本地软件验证，不把历史 ARM64/QEMU 或云端结论移植到新二进制。

# MQMGateway 原始架构审计

## 1. 审计范围与基线

本文只描述上游项目当前实现，不包含本项目后续计划新增的 CAN、统一内部消息、固定线程池、有界队列、指标与故障注入功能。

- 上游仓库：`BlackZork/mqmgateway`
- 审计提交：`b6f24d3b2c4596a19082e584326df70adffedbb6`
- 提交主题：`Merge remote-tracking branch 'origin/ci_tests'`
- 提交时间：`2026-09-06T18:18:39+02:00`
- 审计阶段：第一阶段，静态源码审计
- 本阶段未修改任何核心源码，唯一新增文件是本文档
- 本阶段未编译、运行或生成性能数据；这些工作属于后续 Linux 基线与测试阶段

## 2. 原项目定位

MQMGateway 是一个 Linux/C++17 多线程服务，将一个 MQTT broker 与一个或多个 Modbus RTU/TCP 网络连接起来。它支持：

- 按 YAML 配置创建 MQTT 对象、命令和 Modbus 网络；
- 周期性读取 Modbus coil、discrete input、holding register、input register；
- 将一个或多个寄存器转换为 MQTT 标量、列表或 JSON 对象；
- 从 MQTT 命令 topic 接收值，经转换后写入 Modbus；
- MQTT 5 request/response 风格的 Modbus RPC；
- 每个 Modbus 网络一个工作线程，主线程和 Modbus 线程之间使用两个无锁阻塞队列；
- 通过动态插件完成寄存器与 MQTT 值之间的双向转换。

## 3. 目录与构建结构

```text
mqmgateway/
├── CMakeLists.txt                 # 顶层依赖、C++17、子项目和安装规则
├── modmqttd/
│   ├── main.cpp                  # 可执行程序入口
│   ├── CMakeLists.txt            # modmqttd 可执行目标
│   └── config.template.yaml      # 配置示例
├── libmodmqttsrv/                 # 服务核心：配置、MQTT、Modbus、调度、队列
├── libmodmqttconv/                # 转换器公共接口和值类型，INTERFACE 库
├── stdconv/                       # 标准转换器动态模块 stdconv.so
├── exprconv/                      # 可选 exprtk 转换器模块 exprconv.so
├── readerwriterqueue/             # moodycamel 阻塞无锁队列的内嵌源码
├── unittests/                     # Catch2 单元/模拟/少量真实服务测试
├── buildtests/                    # 容器化构建检查脚本
├── scripts/                       # 发布与 clang lint 脚本
├── Dockerfile
├── docker-compose.yml
└── modmqttd.service              # systemd unit
```

### 3.1 CMake 目标

```text
libmodmqttconv (INTERFACE headers)
          │
          ├── stdconv (MODULE, always built)
          └── exprconv (MODULE, only when exprtk.hpp is found)

libmodmqttsrv (SHARED)
          │
          ├── modmqttd (executable)
          └── tests (executable, unless WITHOUT_TESTS=ON)
```

顶层 `CMakeLists.txt` 要求 CMake 3.13、C++17，并发现以下外部依赖：

- `libmodbus`（pkg-config，必需）
- `libmosquitto`（pkg-config，必需）
- `Threads/pthread`（必需）
- `yaml-cpp`（必需）
- `RapidJSON`（必需）
- `spdlog`（必需）
- `fmt`（必需）
- `Catch2`（启用测试时必需）
- `exprtk.hpp`（可选，存在时构建 `exprconv`）

顶层还设置 `CMAKE_EXPORT_COMPILE_COMMANDS=ON`、`-Wall`、格式安全检查，并通过 `cmake/git_version.cmake` 从 Git 生成版本信息。

## 4. 进程、线程和生命周期

### 4.1 启动顺序

```text
进程装载
  └─ 全局构造 ModMqtt server
       ├─ Mosquitto::libInit()
       ├─ 创建 MqttClient（默认再创建 Mosquitto 实现）
       └─ 创建 ModbusFactory

main()
  ├─ 设置线程名 main
  ├─ 解析 --config / --loglevel / --version
  ├─ server.init(...)
  │    ├─ 初始化日志与转换器插件
  │    ├─ 初始化 MQTT 配置
  │    ├─ 为每个 Modbus network 创建 ModbusClient 和工作线程
  │    ├─ 解析 MQTT objects/commands
  │    ├─ 合并轮询区间
  │    └─ 把最终轮询规格发给相应 Modbus 线程
  └─ server.start()
       ├─ 首先等待 MQTT 连接建立
       ├─ 主线程等待各 Modbus→主线程队列的通知
       ├─ 分发 Modbus 结果给 MqttClient
       └─ 收到停止信号后依次停止 Modbus 线程和 MQTT
```

注意：文件级全局变量 `modmqttd::ModMqtt server` 在进入 `main()` 前构造，因此 libmosquitto 全局初始化、`MqttClient` 和默认 `ModbusFactory` 的创建也发生在 `main()` 前。

### 4.2 线程模型

| 执行上下文 | 创建者 | 主要职责 | 与其他线程的连接 |
|---|---|---|---|
| 主线程 | 操作系统 | 初始化、等待信号/队列、消费所有 `mFromModbusQueue`、驱动关闭流程 | 条件变量 + 每个网络的双队列 |
| Mosquitto 网络线程 | `mosquitto_loop_start()` | 连接/断开/消息/发布回调；直接调用 `MqttClient` 回调 | MQTT 下行时直接向 `mToModbusQueue` 入队 |
| 每个 Modbus 网络一个线程 | `ModbusClient::start()` | 串行连接、轮询、读写、重试、watchdog 与调度 | 消费 `mToModbusQueue`，生产 `mFromModbusQueue` |

原实现没有通用 worker pool。线程数量基本为：主线程 1 个 + Mosquitto 内部线程 1 个 + 每个 Modbus network 1 个。

### 4.3 停止顺序

1. `SIGTERM` 或测试调用 `ModMqtt::stop()` 唤醒主线程。
2. 主线程对每个 `ModbusClient` 调用 `stop()`。
3. `ModbusClient` 向 `mToModbusQueue` 放入 `EndWorkMessage` 并 `join()` 对应线程。
4. 主线程最后一次排空 `mFromModbusQueue`。
5. 若 MQTT 已连接，发布 Modbus 网络不可用状态。
6. `MqttClient::shutdown()` 请求断开，等待 Mosquitto 回调完成。
7. `ModMqtt` 析构时先释放 `MqttClient`，再让动态转换器插件随成员销毁，最后调用 `mosquitto_lib_cleanup()`。

## 5. 核心对象：谁创建、谁持有谁

| 对象/类型 | 谁创建 | 谁持有 | 输入 | 输出/副作用 |
|---|---|---|---|---|
| `ModMqtt` | `main.cpp` 文件级全局对象 | 进程静态生命周期 | CLI 指定的 YAML 路径或测试传入的 YAML node | 创建并协调 MQTT/Modbus/转换器；运行主循环 |
| `MqttClient` | `ModMqtt` 构造函数 | `ModMqtt::mMqtt` (`shared_ptr`) | broker 配置、MQTT 回调、Modbus 返回消息 | MQTT 订阅/发布；向 ModbusClient 下发读写 |
| `IMqttImpl` | 默认由 `MqttClient` 创建 `Mosquitto`；测试可注入 mock | `MqttClient::mMqttImpl` (`shared_ptr`) | client id、broker 配置、subscribe/publish 请求 | 调用 libmosquitto；把事件回调给 `MqttClient` |
| `Mosquitto` | `MqttClient` 构造函数 | 作为 `IMqttImpl` 被 `MqttClient` 持有 | `MqttBrokerConfig` 与 MQTT 数据 | libmosquitto 网络 I/O 与回调 |
| `ModbusFactory` | `ModMqtt` 构造函数 | `ModMqtt` 的静态 `shared_ptr<IModbusFactory>` | network name（默认实现不使用名称） | 每次返回一个新 `ModbusContext`；测试可替换工厂 |
| `ModbusClient` | `ModMqtt::initModbusClients()`，每网络一个 | `ModMqtt::mModbusClients` (`vector<shared_ptr>`)；引用也复制给 `MqttClient` | 网络配置、写命令、RPC、MQTT 在线状态 | 管理线程和两个跨线程队列 |
| `ModbusThread` | `ModbusClient::start()` | `ModbusClient::mThreadImpl` (`unique_ptr`) | `mToModbusQueue` 中的配置/命令/控制消息 | 执行真实 Modbus I/O，结果写入 `mFromModbusQueue` |
| `ModbusContext` | `ModbusFactory::getContext()` | `ModbusThread::mModbus` (`shared_ptr<IModbusContext>`) | RTU/TCP 配置与 `RegisterPoll/RegisterWrite` | 调用 libmodbus 读写；返回寄存器数组或抛异常 |
| `ModbusScheduler` | `ModbusThread` 成员 | `ModbusThread` | 合并后的轮询规格、时间点、成功 RPC 读 | 到期寄存器集合与下一等待时间 |
| `ModbusExecutor` | `ModbusThread` 成员 | `ModbusThread` | 调度出的轮询、写命令、一次性 RPC 读 | 执行、重试、产生值/失败消息 |
| `ModbusWatchdog` | `ModbusThread` 成员 | `ModbusThread` | 已执行命令结果、watchdog 配置 | 决定是否断开并重连 Modbus 网络 |
| `MqttObject` | `ModMqtt::parseObject()` | 初始化后以 `shared_ptr` 被 `MqttClient` 的映射持有 | YAML object、寄存器更新 | state/availability payload 与发布决策 |
| `MqttObjectCommand` | `ModMqtt::parseObjectCommand()` | `MqttClient::mCommands` 按 topic 保存值副本 | topic payload | 目标网络/从站/寄存器和转换器定义 |
| `DataConverter` | 转换器插件工厂 | `MqttObjectDataNode` 或 `MqttObjectCommand` (`shared_ptr`) | `ModbusRegisters` 或 `MqttValue` | 反向转换为 `MqttValue` 或寄存器集合 |
| `MqttValue` | 转换器或 MQTT payload 解析代码 | 按值传递/成员保存 | int/double/int64/uint64/long double/binary | 类型化取值与字符串化 |
| `QueueItem` | 所有跨主线程/Modbus线程消息发送点 | moodycamel 队列按值保存 | `QueueItem::create(T)` 在堆上复制 `T` | 运行时 type hash + 一次性 `getData<T>()` 转移所有权 |

## 6. 关键模块说明

### 6.1 `ModMqtt`

`ModMqtt` 是组合根和主线程协调器。它负责：

- 初始化日志、动态转换器插件、MQTT broker；
- 读取所有 Modbus network/slave 配置；
- 创建一个网络对应一个 `ModbusClient`；
- 解析 MQTT object、state、availability、command 和 RPC 模式；
- 将 YAML 中重复或重叠的寄存器需求合并成轮询规格；
- 构建“寄存器范围 → 受影响 MqttObject”和“command id → 受影响 MqttObject”映射；
- 在主线程消费 Modbus 结果并交给 `MqttClient`；
- 处理停止信号与关闭顺序。

它不直接执行 MQTT socket I/O，也不直接调用 libmodbus。

### 6.2 `MqttClient`

`MqttClient` 是 MQTT 领域逻辑层，而不是底层网络实现。其输入包括：

- `Mosquitto` 的连接、断开和消息回调；
- 主线程从 Modbus 队列取出的寄存器值、读写失败和网络状态；
- 初始化阶段建立的 object、command 和网络映射。

其输出包括：

- 通过 `IMqttImpl` 订阅 command/RPC topic；
- 发布 state、availability 和 RPC response；
- 将 MQTT command/RPC 转成 `MsgRegisterValues` 或 `MsgRegisterReadRequest`，送入目标 `ModbusClient`。

### 6.3 `IMqttImpl` 与 `Mosquitto`

`IMqttImpl` 将 MQTT 业务与库实现分开，测试可注入 mock。`Mosquitto` 是生产实现：

- `mosquitto_connect_async()` 建立异步连接；
- `mosquitto_loop_start()` 启动 libmosquitto 内部线程；
- QoS 当前固定为 0；
- 普通 state/availability 可以 retain；RPC response 不 retain；
- 断线后通过 `mosquitto_reconnect()` 重连；libmosquitto 重连延迟配置为 3 到 60 秒、指数退避；
- MQTT 5 模式用于 RPC response topic、correlation data 和错误 user property。

### 6.4 `ModbusFactory`、`IModbusContext` 与 `ModbusContext`

`IModbusContext` 抽象实际 Modbus I/O，`IModbusFactory` 使测试能按网络注入模拟 context。默认 `ModbusFactory` 忽略传入的 network name，每次创建独立 `ModbusContext`。

`ModbusContext` 包装 libmodbus：

- RTU：`modbus_new_rtu()`，支持 RS232/RS485 模式、RTS 模式与 RTS delay；
- TCP：`modbus_new_tcp()`；
- coil：`modbus_read_bits()` / `modbus_write_bit(s)`；
- discrete input (`BIT`)：`modbus_read_input_bits()`；
- holding register：`modbus_read_registers()` / `modbus_write_register(s)`；
- input register：`modbus_read_input_registers()`；
- 读写错误被转换为 `ModbusReadException` / `ModbusWriteException`。

### 6.5 `ModbusClient` 与 `ModbusThread`

`ModbusClient` 是主线程侧句柄，公开队列与发送方法，并负责线程创建/停止。`ModbusThread` 是工作线程实际实现：

- 消费网络配置、slave 配置、轮询规格、读写命令和 MQTT 在线状态；
- 连接/重连 Modbus 网络；
- 只在 MQTT 在线且已有轮询规格时开始轮询；
- 使用 `ModbusScheduler` 决定哪些寄存器到期；
- 使用 `ModbusExecutor` 在不同 slave 和读写队列间调度，执行延迟与重试；
- 通过 `ModbusWatchdog` 检测长期失败或 RTU 设备移除，触发重连；
- 将值、失败和网络状态发回主线程。

### 6.6 `MqttObjectCommand`

`MqttObjectCommand` 继承 `ModbusMessageBase`，保存：

- 唯一正整数 command id；
- 完整 MQTT topic（`对象基础 topic/命令名`）；
- 目标 Modbus 网络、slave、寄存器类型、地址和数量；
- write mode；
- 可选 `DataConverter`。

目前普通 command 的 payload type 只支持 string/binary 文本。收到消息后，`MqttClient` 用显式 converter 或 `DefaultCommandConverter` 生成 `ModbusRegisters`。

### 6.7 `MqttValue`

`MqttValue` 是转换器边界上的值容器，支持：

- `INT`、`INT64`、`UINT64`；
- `DOUBLE`、`FLOAT64`（内部为 `long double`）；
- `BINARY`（MQTT payload/string）；
- 范围检查后的数值提取和字符串格式化。

它不是整个网关的统一消息模型：没有 timestamp、device id、protocol、direction、quality 等元数据。后续统一模型不能简单等同于 `MqttValue`。

### 6.8 converter 插件层

`DataConverter` 定义两个方向：

```text
ModbusRegisters --toMqtt()--> MqttValue
MqttValue       --toModbus()--> ModbusRegisters
```

`ConverterPlugin` 根据名称创建转换器实例。插件由 YAML 中的 `converter_plugins` 动态加载，并校验 `CONVERTER_ABI_VERSION`。原仓库提供：

- `stdconv`：整数、浮点、字符串、bit、scale、map 等标准转换；
- `exprconv`：可选的 exprtk 表达式转换。

### 6.9 `MsgRegisterValues`

`MsgRegisterValues` 继承 `ModbusMessageBase`，既用于：

- Modbus 读成功后向主线程传递值；
- MQTT command/RPC write 向 Modbus 线程传递待写值；
- 写成功后的乐观回显。

它包含寄存器集合、write mode、command id、创建时间，以及读操作开始时间。command id 为正数表示配置命令；RPC 使用负数；无 command id 表示常规轮询。

### 6.10 `QueueItem`

`QueueItem` 是手工类型擦除的跨线程信封：

- `create<T>()` 在堆上复制消息并保存 `typeid(T).hash_code()`；
- 消费者用 `isSameAs(typeid(T))` 分支，再用 `getData<T>()` 取得 `unique_ptr<T>`；
- `getData()` 只能调用一次。

它没有显式析构释放尚未消费的 `mItem`，也没有现代 variant/RAII 语义。现有正常路径依赖每个 item 最终被准确分发并调用一次 `getData()`。

## 7. 两个方向的队列

每个 `ModbusClient` 都拥有一对独立队列，队列实现为 `moodycamel::BlockingReaderWriterQueue<QueueItem>`。

### 7.1 `mToModbusQueue`：主/Mosquitto → Modbus 线程

| 消息类型 | 生产者 | 消费后的动作 |
|---|---|---|
| `ModbusNetworkConfig` | `ModbusClient::start()` | 创建并配置 `IModbusContext`、executor、watchdog |
| `ModbusSlaveConfig` | `ModMqtt::initModbusClients()` | 保存 slave 级延迟/重试/write mode |
| `MsgRegisterPollSpecification` | `ModMqtt::init()` | 建立 scheduler/executor 的轮询集合 |
| `MsgRegisterValues` | `MqttClient` 经 `ModbusClient` | 转成 `RegisterWrite` 并排队执行 |
| `MsgRegisterReadRequest` | MQTT RPC | 创建一次性 `RegisterPoll` |
| `MsgMqttNetworkState` | MQTT connect/disconnect 回调 | 启停 Modbus 周期轮询 |
| `EndWorkMessage` | `ModbusClient::stop()` | 结束该 Modbus 工作线程 |

### 7.2 `mFromModbusQueue`：Modbus 线程 → 主线程

| 消息类型 | 含义 | 主线程处理 |
|---|---|---|
| `MsgRegisterValues` | 轮询值、RPC read 结果或 write 成功回显 | 更新关联对象并发布 state，或发布 RPC response |
| `MsgRegisterReadFailed` | 读失败 | 更新 availability/状态，或发布 RPC error |
| `MsgRegisterWriteFailed` | 写失败 | 更新关联对象，或发布 RPC error |
| `MsgModbusNetworkState` | Modbus 网络连接/断开 | 更新该网络关联对象的 availability |

向 `mFromModbusQueue` 入队后，`ModbusThread::sendMessageFromModbus()` 会调用全局 `notifyQueues()`；主线程通过全局 mutex/condition variable 被唤醒，然后遍历并排空所有网络队列。

两个队列当前均未设置容量上限，也没有 depth、peak、rejected 或 latency 指标。

## 8. MQTT 上行链路（Modbus → MQTT）

```text
ModbusScheduler
  → 选出到期 RegisterPoll
ModbusExecutor
  → IModbusContext::readModbusRegisters()
  → libmodbus 读取真实 RTU/TCP 数据
  → 产生 MsgRegisterValues
  → mFromModbusQueue
  → notifyQueues()
主线程 ModMqtt::processModbusMessages()
  → MqttClient::processRegisterValues()
  → 用 network/slave/type/address 查找受影响 MqttObject
  → MqttObject 更新寄存器缓存和 availability
  → MqttPayload + DataConverter 生成 payload
  → IMqttImpl::publish()
  → Mosquitto::publish()
  → broker
```

发布规则：

- 默认 `on_change`：payload 改变时发布；
- `every_poll`：按轮询读起始时间和 refresh 节奏强制重发；
- `once`：只发一次；
- state 默认 retain，可按对象配置；
- availability 使用 `<base topic>/availability`，值为 `1` 或 `0`，retain；
- state 使用 `<base topic>/state`。

## 9. MQTT 下行链路（MQTT → Modbus）

### 9.1 配置命令

```text
broker
  → Mosquitto 网络线程 on_message
  → MqttClient::onMessage(topic, payload)
  → 按完整 topic 查找 MqttObjectCommand
  → payload 构造成 MqttValue
  → command converter/default converter 转为 ModbusRegisters
  → 查找目标 ModbusClient
  → ModbusClient::sendCommand()
  → MsgRegisterValues
  → mToModbusQueue
  → ModbusThread::processWrite()
  → ModbusExecutor::addWriteCommand()
  → IModbusContext::writeModbusRegisters()
  → libmodbus 写 RTU/TCP
```

写成功时，executor 将写入值作为确认消息发回主线程；这不是从设备重新读取的确认。若命令覆盖某个已配置的 state 对象，回显可更新并发布该对象状态。

### 9.2 MQTT 5 RPC

当 YAML 将 `mqtt.rpc.mode` 设为 `read` 或 `readwrite` 时，客户端订阅：

```text
<client_id>/rpc/modbus_request
```

请求携带 response topic 与 correlation data。JSON 指定 network、slave、register、register type、count，以及可选 value/converter。`MqttClient` 为请求分配负 command id，记录 `PendingRpcRequest`，向目标 Modbus 队列发送一次性读或写。成功结果通过 MQTT 5 response topic 返回；失败使用空 payload 和 `error` user property。

## 10. Modbus 数据流与调度

### 10.1 配置到轮询规格

1. MQTT object 的 state/availability 声明被解析成寄存器需求。
2. `MsgRegisterPollSpecification::merge/group` 合并相邻或重叠读取，避免同一寄存器为多个 topic 重复轮询。
3. Modbus network/slave 下的 `poll_groups` 只定义允许合并的批量范围；若没有 MQTT 需求与其重叠，该组不会被实际轮询。
4. 最终规格经 `mToModbusQueue` 交给对应网络线程。

### 10.2 工作线程内调度

- `ModbusScheduler` 根据 refresh 和上次读时间返回当前到期集合及下次唤醒时间；
- `ModbusExecutor` 为每个 slave 维护 poll/write 队列；
- 零积压时，新写命令会优先于轮询，以降低写延迟；
- executor 在 slave 间轮换并限制连续命令数量，降低饥饿风险；
- 网络与 slave 可分别配置 command delay、slave 切换后的 first-command delay、读写重试次数；
- 一次性成功 RPC 读若与周期轮询范围完全一致，会推进该轮询时间，避免同一 refresh 周期重复读取。

### 10.3 连接与错误路径

- Modbus 未连接时重试间隔逐步增加，每次增加 5 秒，上限逻辑为 60 秒；
- 读失败由 executor 按配置重试；普通轮询持续错误后向主线程发 `MsgRegisterReadFailed`；
- 写失败按配置重试，并发 `MsgRegisterWriteFailed`；
- watchdog 在规定时间内没有成功命令，或发现 RTU 设备路径消失时，强制断开并重连；
- 网络状态改变会产生 `MsgModbusNetworkState`，最终影响 MQTT availability。

## 11. YAML 配置如何映射到运行时对象

顶层结构为：

```yaml
modmqttd:
  log_level: debug
  converter_search_path: [...]
  converter_plugins: [...]

modbus:
  networks:
    - name: network_name
      # RTU: device/baud/parity/data_bit/stop_bit
      # 或 TCP: address/port
      slaves:
        - address: 1,2,5-10
          poll_groups: [...]

mqtt:
  client_id: modbus
  refresh: 10s
  publish_mode: on_change
  rpc:
    mode: disabled | read | readwrite
  broker:
    host: localhost
    port: 1883
    keepalive: 60
  objects:
    - topic: device/${slave_address}
      network: network_name
      slave: 1
      state: ...
      availability: ...
      commands: ...
```

关键映射：

| YAML | 运行时结果 |
|---|---|
| `modmqttd.converter_plugins` | 加载 `ConverterPlugin` 动态模块 |
| `modbus.networks[]` | 每项创建一个 `ModbusClient`、一个 `ModbusThread` 和一个 context |
| `networks[].slaves[]` | 生成一个或多个 `ModbusSlaveConfig` 消息 |
| `mqtt.broker` | `MqttBrokerConfig`，传给 `MqttClient/Mosquitto` |
| `mqtt.objects[]` | 每个 network/slave 展开后创建一个 `MqttObject` |
| `state` | 轮询需求 + payload 数据树 |
| `availability` | 可用性判断所需寄存器与期望值 |
| `commands` | topic 到 `MqttObjectCommand` 的映射 |

topic 可使用 `${network}`、`${slave_address}` 和 `${slave_name}` 占位符。十进制寄存器地址按用户习惯视为 1-based 并在解析时减一；`0x` 十六进制地址直接视为 0-based。

## 12. 原始架构边界与已确认风险

以下是源码中已经存在的边界或 TODO，不是本项目新增问题：

1. **队列无界。** `mToModbusQueue` 和 `mFromModbusQueue` 没有容量、背压、拒绝策略或指标；源码已有相关 TODO。
2. **没有通用 worker pool。** 每个网络串行执行其 Modbus I/O；CPU 型转换和 MQTT 业务不经过固定线程池。
3. **断线时的数据策略是丢弃而非持久重试。** `MqttClient::processRegisterValues()` 在 MQTT 未连接时直接丢弃变化，重连后仅重新发布内存中的当前对象状态。
4. **QoS 固定为 0。** 普通 publish、subscribe 和 RPC response 都未配置 QoS 1/2；没有应用级 publish acknowledgement/retry 队列。
5. **共享状态同步边界不完整。** 源码明确标注 `mConnectionState` 会被 Mosquitto 回调线程和主线程共同访问，但不是 atomic，也没有专用锁。
6. **Mosquitto 回调直接遍历 Modbus client 列表。** 初始化后列表通常不再变化，但源码仍保留线程安全 TODO。
7. **`QueueItem` 是裸指针类型擦除。** 未消费或错误分发的 item 缺少自动释放保障，后续扩展消息类型时风险较高。
8. **启动依赖 MQTT。** 初始 MQTT 未连接时主循环等待；Modbus 线程也不会开始周期轮询，以避免上行队列无限增长。
9. **写成功采用乐观回显。** 写后的 `MsgRegisterValues` 是请求值而非设备回读值，不能当作物理设备已应用值的强确认。
10. **错误隔离粒度主要是网络/线程。** 单个 slave 的持续错误由重试和 watchdog 处理，但 watchdog 可能令整个 Modbus context 重连。
11. **缺少目标系统要求的通用数据模型。** `MqttObject`、`MsgRegisterValues` 和 `MqttValue` 都是专用类型，尚无 protocol/direction/quality/device_id 完整消息封装。
12. **尚无 CAN/SocketCAN。** 原树中不存在 CAN 模块或 vcan 测试。
13. **尚无统一真实性能体系。** 没有标准化吞吐、端到端延迟、队列深度、CPU/RSS、丢失率和重连时间原始数据输出。

这些边界说明后续应采用适配器和新增中间层逐步扩展，同时保留现有 MQTT、Modbus、converter 和轮询调度的稳定部分。

## 13. 后续阶段的兼容性约束

后续设计至少应保持以下原始行为可回归验证：

- 现有 YAML 的 Modbus RTU/TCP 与 MQTT object 配置继续可用；
- `state`、`availability`、`commands` topic 语义不被静默改变；
- 标准与 exprtk converter 的双向接口保持兼容，或提供明确 ABI 迁移；
- 一个 network 一个串行 Modbus 执行上下文的设备总线安全性保持不变；
- 合并轮询、publish mode、slave delay、重试和 watchdog 行为有回归测试；
- 测试注入点 `IMqttImpl` 与 `IModbusFactory/IModbusContext` 保留；
- 新的有界队列、统一消息和线程池必须明确处理所有现有 `QueueItem` 消息类型与关闭顺序。

## 14. 第一阶段结论

原项目不是简单的协议转发器，而是已经具备清晰分层的双向 Modbus/MQTT 服务：`ModMqtt` 负责装配和主循环，`MqttClient` 负责 MQTT 领域逻辑，`Mosquitto` 与 `ModbusContext` 分别封装外部协议库，每个 `ModbusClient/ModbusThread` 通过双队列与主线程解耦，`Scheduler/Executor/Watchdog` 管理轮询、执行、重试和恢复，converter 插件处理数据表示。

后续目标与原架构的主要冲突不在基本协议能力，而在通用化和可观测性：需要新增 CAN、统一消息、路由、固定线程池、有界背压、可靠发布和真实性能证据。最小侵入方向应是保留现有协议适配与配置解析，在队列边界引入兼容的统一消息/指标层，再逐步增加 CAN adapter 和测试基础设施。

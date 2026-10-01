# mqmgateway 源码级面试档案

> 审计对象：当前仓库 `main` 分支工作树；证据限定为当前源码、测试、`docs/`、`results/` 与本地 Git 历史。此文档是阅读地图，不替代运行记录。路径后的 `:数字` 为当前文件行号；代码变化后应重新定位。**当前源码未体现 / 无法从仓库确认**的事情不按惯例推断。

## 0. 先分清两个进程与一套独立串口库

顶层 `CMakeLists.txt:51-56` 同时构建旧服务 `modmqttd`、新可执行程序 `mqmgateway_iot` 和串口库 `mqmgateway_serial`。`docs/architecture.md`、`docs/PROVENANCE.md` 明确说明：Modbus RTU/TCP 仍由上游 `modmqttd` 处理；SocketCAN、统一消息、有界队列和 worker 属于新增的**另一个进程**。两者通过各自 MQTT 客户端连接 broker，没有进程内共享 `UnifiedMessage`。`src/serial/termios_rtu_transport.cpp` 的帧解析器由 `tests/serial/rtu_transport_tests.cpp` 与独立测试目标使用（`src/serial/CMakeLists.txt:1-16`）；**当前源码未体现它接入 `modmqttd` 或 `mqmgateway_iot` 的生产数据链路**。原 Modbus RTU 在 `libmodmqttsrv/modbus_context.cpp:14-39` 交给 libmodbus，不能把独立解析器描述成旧服务的内部实现。

面试开场可说：这是保留上游 Modbus–MQTT 服务、再增加独立 CAN–MQTT 进程的组合；Buildroot/QEMU、PTY、vcan、真实 EMQX Cloud 分别验证了不同边界。来源证据见 `docs/PROVENANCE.md`；不要声称整套 Modbus 核心从零开发。

## 1. 真实入口、配置、启动与退出

### CAN 进程 `mqmgateway_iot`

`src/iot_gateway/main.cpp:46-140`：栈上构造 `GatewayConfig`，依次解析 CLI、调用 `applyCloudEnvironment`（环境覆盖 host/port/TLS 等）、注册 SIGINT/SIGTERM、栈上构造 `Gateway`、调用 `start()`，主线程每 100 ms 查看 `stopRequested`，再 `stop()`。这里没有 YAML 配置加载。`GatewayConfig` 默认值在 `gateway.hpp:22-42`：CAN `vcan0`、队列 1024、worker 2、MQTT 127.0.0.1:18883、keepalive 10 秒、遥测 QoS1、inflight 20。

```text
main.cpp::main
  → CLI + applyCloudEnvironment → GatewayConfig
  → Gateway 构造：成员 CanSocket、CommandRouter、BoundedQueue
  → Gateway::start：mosquitto_lib_init → can_.open(socket/bind/epoll)
    → mosquitto_new → 认证/TLS/回调/重连配置 → connect_async → loop_start
    → receiver_ → workers_[0..N-1] → heartbeat_
  → MQTT callback 入队 / CAN receiveLoop 入队 → workerLoop → CAN send 或 MQTT publish
  → 信号 → Gateway::stop：queue_.stop → join receiver/heartbeat/workers
    → offline publish → disconnect/loop_stop/destroy → can_.close → metrics → lib_cleanup
```

对应 `src/iot_gateway/gateway.cpp:15-124,133-339`。构造函数只校验配置；`CanSocket::open` 在 `start` 执行。**启动异常边界**：`can_.open()` 或认证/TLS配置中途抛异常时 `running_` 仍为 true，栈展开后的析构会调用 `stop()`；`mosquitto_new`、`connect_async`、`loop_start` 的某些失败分支先置 `running_=false`，析构中的 `stop()` 因而直接返回。`CanSocket` 成员析构仍会关它自己的 fd，但 `mqtt_` 和 libmosquitto 的中途初始化**没有统一回滚保证**（`gateway.cpp:25-29,44-125`）。正常退出顺序如上。`wait()` 存在但 `main` 未调用（`gateway.cpp:127-131`）。

### Modbus 进程 `modmqttd`

`modmqttd/main.cpp:18,35-95` 有全局 `ModMqtt server`，CLI `--config/--loglevel` 后 `server.init()`、`server.start()`。`ModMqtt` 构造时初始化 Mosquitto 库、`MqttClient` 和默认 `ModbusFactory`（`libmodmqttsrv/modmqtt.cpp:164-172`）。`init(int,path)` 加载 `YAML::LoadFile`，空路径用 `./config.yaml`（`:175-193`）。`init(YAML)` 顺序：`initServer` → `initBroker` → `initModbusClients` → `initObjects` → 合并 poll specification → 设置 MQTT 的对象索引（`:195-269`）。`initModbusClients` 创建各网络 `ModbusClient`，并在配置阶段调用 `start`；因此 Modbus 网络线程**早于** `ModMqtt::start()` 中的 MQTT 初始连接启动。它收到配置消息后可尝试连接 Modbus，但 `ModbusThread::run` 用 `mMqttConnected` 门控轮询（`modbus_thread.cpp:231-337`）。

```text
modmqttd/main.cpp::main → 全局 ModMqtt
  → init：YAML → broker/network/object/poll/command 配置
    → 每网络 ModbusClient::start → std::thread(ModbusThread::run)
  → start：MqttClient::start → Mosquitto::connect → mosquitto_loop_start
    → 等首次 broker 连接 → 主线程 waitForQueues/processModbusMessages
  → SIGTERM 或测试 stop → join 各 ModbusClient
    → 清空来自 Modbus 的消息 → availability 下线 → MQTT shutdown
```

证据：`libmodmqttsrv/modmqtt.cpp:940-1057`、`modbus_client.cpp:9-29`、`mosquitto.cpp:116-190`。`ModMqtt::start` 注册 SIGTERM；代码有 SIGHUP 分支但只有 TODO（`:965-982`），不能称支持热重载。`ModMqtt::~ModMqtt` 的库清理见 `:1109-1115`。`MqttClient` 回调由 `mosquitto_loop_start` 所启动的 libmosquitto 循环处理；不能把所有 MQTT 回调笼统说成旧服务主线程工作。

## 2. 20 个核心类/结构体：对象与所有权速查

“跨线程”表示实例或经引用访问跨线程，不表示其所有成员均可并发调用。输入/输出列出最重要的业务边界。

| 类/结构体与文件 | 创建、持有、所有权、生命周期 | 职责；重要成员/函数；输入 → 输出；跨线程/安全边界 |
|---|---|---|
| `Gateway` `gateway.hpp:44` | `main` 栈对象；析构调用 `stop` | CAN/MQTT 进程编排；成员 `can_/router_/queue_/mqtt_/receiver_/heartbeat_/workers_`；`start/stop/onMessage/receiveLoop/workerLoop/publish`。MQTT callback、receiver、workers、heartbeat 共用它；部分字段 atomic，不能推断所有访问无竞态。 |
| `CanSocket` `can_socket.hpp:15` | `Gateway` 值成员；析构 `close` | 持有 `socket_`/`epoll_` 原始 fd；`open/receive/send/close`。receiver 读、workers 写；观测计数 atomic、`parseLatency_` 带锁；关闭前 join。 |
| `CommandRouter` `command_router.hpp:17` | `Gateway` 值成员；同寿命 | 无可变成员；`route(topic,payload)` 从 MQTT callback 解析成 `RouteResult`，非法输入给 `error`。 |
| `UnifiedMessage` `unified_message.hpp:15` | CAN `receive` 或 `route` 构造；经 `BoundedQueue` 按值移动；worker 局部持有 | `deviceId/protocol/direction/dataType/payload/address/enqueuedAt/timeout`；`toJson` 输出遥测 JSON；值在队列锁下转移，worker 独占取出的值。 |
| `BoundedQueue<UnifiedMessage>` `bounded_queue.hpp:24` | `Gateway` 值成员；构造时给容量，Gateway 析构销毁 | `std::deque`、`mutex_`、`condition_`、`stopped_`、`metrics_`；callback/receiver 生产，workers 消费；`tryPush/pop/stop/metrics` 均在锁下；满即拒绝。 |
| `PublishTracker` `pipeline_metrics.hpp:59` | Gateway 值成员；默认容量 4096 | 以 MID 关联 publish 接受与 callback/PUBACK，`recursive_mutex` 保护 `pending_/early_`；`submit/completed/write`；**诊断跟踪，不是 MQTT 流控**。 |
| `RtuFrameParser` `src/serial/termios_rtu_transport.hpp:21` | `TermiosRtuTransport::parser_` 值成员；测试也单独栈上建 | `buffer_`/metrics；`feed/candidateLengths/validCrc/discardPrefix` 将字节流切为 CRC 合法候选；当前未见跨线程同步。 |
| `TermiosRtuTransport` `termios_rtu_transport.hpp:42` | 测试里栈上构造；生产入口未构造 | `descriptor_` 原始 fd、`parser_`；`open/readFrames/writeFrame/close`；PTY 测试输入/输出 RTU 帧。**不在生产链路**。 |
| `ModMqtt` `libmodmqttsrv/modmqtt.hpp:24` | `modmqttd/main.cpp:18` 全局静态；测试也可创建；成员 `shared_ptr<MqttClient>`、`vector<shared_ptr<ModbusClient>>` | YAML 编排与主线程消息分发；`init/start/processModbusMessages/stop`。由主线程和 Mosquitto 回调间接共享，队列/条件变量通信。 |
| `MqttClient` `mqttclient.hpp:18` | `ModMqtt` 构造中 `shared_ptr` 创建与持有 | broker 状态、命令映射、对象映射、RPC；`onMessage/onConnect/processRegisterValues/publishState`。`mMqttImpl` 是 `shared_ptr<IMqttImpl>`；状态字段在头文件有线程上下文 TODO（`:88-91`），不能称整体线程安全。 |
| `Mosquitto` `mosquitto.hpp:14` | `MqttClient` 构造中 `shared_ptr<IMqttImpl>`，实际为 `Mosquitto` | 持有 `mosquitto* mMosq` 原始库句柄；`connect/subscribe/publish/stop`；将库回调送回 `MqttClient`。库循环与主线程均触及包装层；库内部锁细节当前源码未体现。 |
| `ModbusClient` `modbus_client.hpp:16` | `ModMqtt::initModbusClients` 建 `shared_ptr`，`ModMqtt` 与 `MqttClient` 保存引用；析构 `stop` | 两个 SPSC `BlockingReaderWriterQueue<QueueItem>`；`unique_ptr<ModbusThread>`、`shared_ptr<std::thread>`；`start/sendCommand/stop`；主/MQTT 方与每网络 Modbus 线程通过队列交换。**不是新增 CAN 有界队列**。 |
| `ModbusThread` `modbus_thread.hpp:14` | `ModbusClient::start` 以 `unique_ptr` 创建，线程 `run`，join 后销毁 | `mModbus/mScheduler/mExecutor/mWatchdog/mSlaves`；消费配置/写入、发 poll/result；网络 I/O 在本线程，复连和 watchdog 在 `run`。 |
| `ModbusContext` `modbus_context.cpp`、`modbus_context.hpp` | `ModbusFactory` 通过 `IModbusContext` 指针提供给 ModbusThread；详情见 `configure` | `modbus_t* mCtx`、连接状态；`modbus_new_tcp/rtu`、`modbus_connect/read_*/write_*/close/free`；fd 被 libmodbus 管理，当前本仓库不直接做该 fd 的 epoll/termios 操作。 |
| `ModbusScheduler` `libmodmqttsrv/modbus_scheduler.hpp:14` | `ModbusThread` 值成员，同寿命；仅 Modbus 网络线程使用 | `mRegisterMap`；`setPollSpecification/getRegistersToPoll/notifyRpcRead`；上游 poll specification，输出到 executor 的到期寄存器表；不跨线程共享。 |
| `ModbusExecutor` `libmodmqttsrv/modbus_executor.hpp:13` | `ModbusThread` 值成员，同寿命；仅其线程执行 | `mModbus/mSlaveQueues/mWaitingCommand/mLastCommand`；`addPollList/addWriteCommand/executeNext`；输入 scheduler/命令，输出 libmodbus 读写及返回主线程的 QueueItem；本类不以锁共享。 |
| `ModbusWatchdog` `libmodmqttsrv/modbus_watchdog.hpp:9` | `ModbusThread` 值成员，同寿命；仅其线程执行 | `mLastSuccessfulCommandTime/mDeviceRemoved`；`inspectCommand/isReconnectRequired/reset`；输入 executor 命令结果，输出是否要断开并重连；本类不以锁共享。 |
| `MqttObject` `libmodmqttsrv/mqttobject.hpp:190` | `ModMqtt::initObjects` 建值，`ModMqtt::init` 转 `shared_ptr` 放 `MqttClient::mObjects/mCommandObjects` | `mState/mAvailability/mStateTopic/mPublishMode/mLastPublishedPayload`；状态值由 `MqttClient::processRegisterValues` 更新，`publishState` 决定输出；对象经 shared_ptr 跨回调/主线程，整体安全性当前源码未证明。 |
| `MqttObjectCommand` `libmodmqttsrv/mqttcommand.hpp:10` | `ModMqtt::parseObjectCommand` 建值，`MqttClient::mCommands` 按 topic 持有，随 MqttClient 结束 | `mTopic/mModbusNetworkName/mWriteMode/mConverter`；MQTT callback 查命令、转换 payload，输出 `ModbusClient::sendCommand`；跨 MQTT 回调与 Modbus 工作路径时通过消息队列传值。 |
| `QueueItem` `libmodmqttsrv/queue_item.hpp:11` | `QueueItem::create` 生成并作为值入旧 SPSC 队列；消费者 `getData<T>` 转 `unique_ptr<T>` 领取内部 raw pointer | `mTypeHash/mItem`；输入配置、读写请求或结果，输出给 `ModbusThread::dispatchMessages`/`ModMqtt::processModbusMessages`；跨线程移动与一次领取，队列负责同步。 |

## 3. 真实线程图与同步

```text
两个独立进程（不是同一 Process）
modmqttd
 ├─ 主线程：main → ModMqtt::start/waitForQueues/processModbusMessages
 ├─ 每个 Modbus network 一个 std::thread：ModbusClient::threadLoop → ModbusThread::run
 └─ libmosquitto loop_start 启动的网络循环线程：MqttClient callbacks

mqmgateway_iot
 ├─ 主线程：main 的 signal/sleep/stop
 ├─ libmosquitto loop_start 的网络循环线程：connected/disconnected/message/published callbacks
 ├─ receiver_：Gateway::receiveLoop → CanSocket::receive(epoll_wait 最长 100 ms)
 ├─ workers_[0..max(1,config.workers)-1]：Gateway::workerLoop → queue_.pop(CV)
 └─ heartbeat_：Gateway::heartbeatLoop → 指标快照与分段 sleep
```

创建点：`libmodmqttsrv/modbus_client.cpp:9-25`、`libmodmqttsrv/mosquitto.cpp:164-173`、`src/iot_gateway/gateway.cpp:83-98`；没有直接 `pthread_create`。`modmqttd` 网络线程的阻塞在 libmodbus I/O、`wait_dequeue_timed`（`modbus_thread.cpp:231-337`）；主线程在 `gHasMessagesCondition.wait`（`modmqtt.cpp:1073-1084`）。MQTT 内部确切 fd、阻塞系统调用、是否再派生辅助线程：**当前源码未体现 / 无法从仓库确认**。新增 CAN 线程的 fd 是 CAN socket 与 epoll fd；worker 使用同一 CAN socket 发送、另调用 MQTT publish；heartbeat 调 MQTT publish 和 metrics 文件。`Gateway::stop` 先 `queue_.stop()` 再 join（`gateway.cpp:100-124`）；queue 的 `pop` 在 stop 后若仍有积压会继续取，但 `workerLoop` 的外层 `while(running_)` 使它可能仅处理当前项后退出，**不保证排空**。`modmqttd` 的 `ModbusClient::stop` 入队 `EndWorkMessage` 后 join。

锁与共享资源逐项：`BoundedQueue::mutex_` 保护 deque、`stopped_` 和 metrics，条件变量只唤醒 worker；`StageLatency::mutex_` 保护桶/计数；`PublishTracker::recursive_mutex` 保护 MID map 和同步回调早到状态（`pipeline_metrics.hpp:64-154`）；`CanSocket` 的计数与 `Gateway` 的状态/计数是 atomic（`can_socket.hpp:33`、`gateway.hpp:77-84`）；旧服务的 `gQueueMutex/gHasMessagesCondition/gHasMessages` 只协调“有消息可读”，实际消息由 SPSC 队列传递（`modmqtt.cpp:62-121,1073-1084`）。旧 `MqttClient::mConnectionState` 头文件自己标了线程上下文 TODO；当前源码未提供整体竞态证明。不能仅凭这些锁断言无死锁：`PublishTracker::submit` 持递归锁调用 `mosquitto_publish`，回调可能同步进入 `completed`，递归锁是对此的显式处理；与 libmosquitto 内部锁的完整锁序**当前源码未体现 / 无法从仓库确认**。另一个具体风险是旧 `signal_handler` 直接调用会加 `gQueueMutex` 的 `notifyQueues`（`modmqtt.cpp:54-63,116-121`）；若信号打断了持锁线程，可能自锁，且 mutex/CV 调用不是异步信号安全操作。当前测试结果不能证明这一极端路径不会发生。

## 4. fd 与 epoll：只在 CAN 路径

`src/iot_gateway/can_socket.cpp:23-58`：`socket(PF_CAN, SOCK_RAW|SOCK_NONBLOCK, CAN_RAW)` 得 `socket_`；`ioctl(SIOCGIFINDEX)` 查配置接口（默认 `vcan0`），`bind` 到 `sockaddr_can`；`epoll_create1(EPOLL_CLOEXEC)` 得 `epoll_`；仅将 `socket_` 以 `EPOLLIN` 注册。没有 `EPOLLET`，因此采用默认 LT；无显式 `EPOLLOUT/EPOLLERR/EPOLLHUP` 处理分支，也没注册串口 fd、eventfd 或 MQTT socket。内核可能报告错误/挂断，但本实现未按 `event.events` 分类处理。

`CanSocket::receive`（`:86-124`）每次 `epoll_wait(epoll_, &event, 1, timeout)`，`ready<=0` 返回 false；`ready>0` 后不检查 `event.data.fd`/事件位，直接单次 `read(socket_, sizeof(can_frame))`。读不到完整 `can_frame` 则返回 false；开启指标时计 `read_errors`。没有显式 `EAGAIN/EINTR` 重试，也没有循环读至 EAGAIN；在 LT 下剩余帧可让下次 `epoll_wait` 继续就绪。`send`（`:74-83`）是单次 `write`，仅以是否写满完整 frame 返回布尔值，无 EPOLLOUT 等待或区分 errno。`close` 先关 epoll 再关 socket（`:60-71`）。这就是源码实际的 `epoll_wait → read → 业务`，不能背出不存在的 fd 分派器。`TermiosRtuTransport` 用 `poll(2)` 而非 epoll（`src/serial/termios_rtu_transport.cpp:208-256`）；旧 Modbus 使用 libmodbus 自己的 I/O。

## 5. Modbus RTU 字节流：独立解析器与真实服务分开答

**独立验证链**：`TermiosRtuTransport::open`（`src/serial/termios_rtu_transport.cpp:171-201`）以 `O_RDWR|O_NOCTTY|O_NONBLOCK|O_CLOEXEC` 打开设备；`tcgetattr`、`cfmakeraw`、9600/19200/38400/57600/115200 波特映射、8N1（清 parity/stop/size，设 CS8）、`VMIN=VTIME=0`、`tcsetattr`、`tcflush`。`readFrames`（`:239-256`）`poll(POLLIN)` 超时返回空，再最多读 512 字节并送 `parser_.feed`；`writeFrame`（`:204-237`）以 `steady_clock` deadline、`poll(POLLOUT)`、循环处理部分写与 EAGAIN/EINTR，最后 `tcdrain`。`read=0` 被送给 `feed(...,0)`，没有单独的 EOF/断开处理。

`RtuFrameParser::feed`（`:104-159`）先追加到 `buffer_`。扫描每个偏移的候选长度：异常码最高位为 1 时长度 5；FC 1–4 有固定 8 字节请求候选与 `byte_count+5` 响应候选；FC 5/6 固定 8；FC 15/16 有固定 8 响应与 `byte_count+9` 请求候选（`:49-87`）。对完整候选用 Modbus CRC16（初值 `0xffff`、多项式 `0xa001`、低字节在前）校验（`:23-47,89-99`）。找到有效候选时先丢弃前缀噪声，取出该帧，继续从缓冲处理下一帧；因此一次 `read` 可以只有半包、多个粘包，或噪声加合法帧。若未找到完整有效候选，保留缓冲等待后续 `feed`；仅当缓冲超过配置上限（最少 256）才丢弃到最后 256 字节（`:142-151`）。所以**CRC 错误不是立即丢弃整个坏帧**，而是扫描后续偏移寻找下一个 CRC 合法候选，或在上限触发丢前缀。`crcCandidatesRejected` 是候选计数，且在找到帧或超限时才累加，不等于线上的坏帧数。`bytesDiscarded/resyncEvents/fragmentedFeeds` 见 `termios_rtu_transport.hpp:14-19`。

`tests/serial/rtu_transport_tests.cpp:33-106` 对分片、粘包、噪声、32 次坏 CRC 后恢复、PTY 收发做断言。`tests/modbus/modbus_rtu_test.sh` 和 `tests/modbus/rtu_simulator.py` 验证原服务经 libmodbus 的 RTU 链；`results/functional/modbus/rtu-frames.csv` 保存请求/应答字节。**当前源码未体现**独立解析器按 Modbus 3.5 字符静默间隔定界，也没有将解析出的帧送入 `ModMqtt`；原服务内部的帧状态机/重同步算法在 libmodbus 外部，**无法从本仓库确认**。

## 6. SocketCAN、统一模型与双向路径

`CanSocket::receive` 在 `can_socket.cpp:95-122` 读 classic `can_frame`；DLC 大于 8 或 `CAN_RTR_FLAG/CAN_ERR_FLAG` 则拒绝。它用 `frame.can_id & CAN_EFF_MASK` 得十进制地址和 `can-{id}` 设备名，`payload` 拷 DLC 字节，标 `Protocol::can/Direction::northbound/DataType::telemetry/Quality::good`。这里没有读取 CAN_EFF_FLAG 后区别标准/扩展帧，也没有设备注册表或物理身份鉴别；`deviceId` 只是从 CAN ID 推导。`CAN_ERR_FLAG` 过滤不等于底层总线错误诊断。

```text
vcan0/指定接口 → CanSocket::receive → UnifiedMessage
 → Gateway::receiveLoop → BoundedQueue::tryPush
 → Gateway::workerLoop → toJson → telemetryTopic → Gateway::publish → broker/EMQX

broker/EMQX → messageCallback → Gateway::onMessage
 → cloud topic 变换（若 --cloud）→ CommandRouter::route
 → BoundedQueue::tryPush → workerLoop timeout 检查
 → CanSocket::send(write CAN frame) → publishStatus(result) → broker/EMQX
```

证据：`gateway.cpp:141-246,278-315`、`command_router.cpp:34-88`、`unified_message.cpp:56-77`。命令 JSON 必须有无符号 `can_id`（<=29 位）和不超过 8 字节的偶数字符 hex `data`；可选 `timeout_ms` 必须正整数。发送端直接将 `message.address` 写入 `frame.can_id`（`can_socket.cpp:79-83`）；它未按大于 11 位的 ID 设置 `CAN_EFF_FLAG`，因此“29 位 ID 都能按预期作为扩展帧发出”**当前源码未体现 / 无法从仓库确认**。软件证据 `tests/can/can_mqtt_integration_test.sh`、`tests/can/send_invalid_can.py`、`results/functional/can/summary.txt` 与云闭环 `results/cloud-live-emqx/` 使用 vcan0；真实收发器、总线电气层、仲裁、物理 CAN 设备身份均**无法从仓库确认**。

## 7–9. Worker、队列与过载保护

`Gateway::start`（`gateway.cpp:93-98`）启动 `max(1, config_.workers)` 个固定 worker；默认 2，可用 `--workers` 改（`main.cpp:75`）。任务是 `UnifiedMessage`，遥测与命令**共用同一个** `queue_`。receiver 与 MQTT callback 用 `tryPush`；worker `pop` 在条件变量上等到任务或 stop；退出见第 3 节。正常遥测的 `toJson/publish` 和命令的 `can_.send` 在 worker 中；但 `onMessage` 对非法命令或满队列会在 MQTT callback 内直接 `publishStatus`，该函数的 `publish` 可能退避等待（`gateway.cpp:175-194,278-299`），所以不能声称回调永不阻塞。为什么固定数量、不一请求一线程：**工程分析**是控制并发与线程资源，源码只证明固定数量；未保存同负载替代设计的对照，不能说性能已被证实最优。

`BoundedQueue`（`bounded_queue.hpp:24-81`）是 `std::deque<T>` + mutex + CV，容量来自 `queueCapacity`。满/已 stop 时 `tryPush` 立即 false 并累加 `rejected`；**reject-new，不阻塞、不等待空位**。`pop` 等待后从队头取；`stop` 标志并 `notify_all`。若容量设 0，所有 push 均拒绝，源码没有校验为正。生产者：CAN receiver、MQTT callback；消费者：所有 workers。无限队列可能随入速持续高于出速而增长内存和排队延迟，这是工程分析；当前有界队列给出显式拒绝代价。旧 `ModbusClient` 两个 moodycamel `BlockingReaderWriterQueue` 在 `modbus_client.hpp:21-58`，其发送代码还有 TODO 限制最大队列，**不能说旧 Modbus 链路也用此有界拒绝策略**。

命令期限不是独立绝对时间字段：`CommandRouter::route` 在 `command_router.cpp:64-85` 设置 `enqueuedAt=steady_clock::now()`、默认 `timeout=1000ms`，可由 JSON 覆盖；`Gateway::workerLoop` 在可选 `processingDelay` 后比较 `steady_clock::now()-enqueuedAt > timeout`（`gateway.cpp:211-247`）。超过则 `commandTimeouts_++` 并发布 `timeout`；未超过则调用 `can_.send`，按布尔值发布 `ok` 或 `error`，并在错误时 `canErrors_++`。`ok` 仅证明单次 CAN `write` 写满一帧，**不是目标设备执行确认**。队列拒绝在 `onMessage` 发布 `rejected`；非法 topic/payload 同样发布 `rejected`，但只有队列拒绝进入队列 `rejected` 指标。遥测队列满则 `canErrors_++`；这个字段也包含发送失败，因此不是纯 CAN 总线错误计数。

`tests/integration/reliability_test.sh:19-93` 用容量 4、1 worker、5ms 人工处理延迟、1ms 命令超时，向 QoS0 topic 发 3000 行命令。记录 `results/fault/reliability/metrics.json`：`enqueued=228, dequeued=224, current_depth=4, rejected=641, command_timeouts=223, peak_depth=4`；`summary.txt` 同步摘录。`3000-641-223=2136` **不能映射为成功**：3000 是生成器尝试写给 broker 的行数，网关指标只记录已抵达并被处理的入队/拒绝；现有结果未保存逐条唯一 ID 与 3000 条结果对账，且 `enqueued=228` 与“2136 成功”矛盾。可靠表述是至少观察到 641 次队列拒绝、223 次 worker 超时；具体成功总数**无法从仓库确认**。

## 10–11. MQTT/TLS/EMQX 数据路径与交付边界

新增 CAN MQTT 封装就在 `Gateway`：`gateway.cpp:44-93` 调 `mosquitto_lib_init/new`、设置最大 inflight、用户名/密码、CA 和 `mosquitto_tls_opts_set(...,1,"tlsv1.2",nullptr)`、回调、`mosquitto_reconnect_delay_set(1,30,true)`、`connect_async`、`loop_start`。`onConnected` 每次连接成功重新订阅命令 topic 并发 retained gateway online（`:149-168`）；`onDisconnected` 清 `connected_`（`:170-173`）。`publish`（`:278-299`）最多 6 次尝试，每次失败后 100、200、400、800、1000、1000 ms 等待；它可从 worker、heartbeat、callback 调用，依赖 libmosquitto API，仓库没有额外的 publish 全局锁。遥测 QoS 来自配置（默认 1、可设 0）；命令订阅和 status/heartbeat QoS1；offline/online retained，遥测与 command/result 不 retained。`published_` 只表示 `mosquitto_publish` API 接受；开 `pipelineMetrics` 时 `PublishTracker` 通过 publish callback 按 QoS1 记录 `puback_received`（`pipeline_metrics.hpp:70-150`），但跟踪表满会丢诊断关联，不改变库自身流控。QoS1 不能推出订阅端必达或恰好一次。

新增进程读取的环境变量名（`src/iot_gateway/main.cpp:24-40`）：`EMQX_HOST`、`EMQX_PORT`、`EMQX_CA`、`GATEWAY_MQTT_USERNAME`、`GATEWAY_MQTT_PASSWORD`、`GATEWAY_MQTT_TLS`。云测试启动脚本还读取 `MQMGATEWAY_EMQX_ENV`、`XDG_CONFIG_HOME`、`HOME` 定位私有环境文件（`tests/cloud/run_sustained_emqx.sh:4`）；它们不是网关进程的云配置项。无密码值写在本文。`--cloud` 强制 TLS，并要求 host 不等于字面量 `127.0.0.1`、用户名非空、用户名/密码同时配置和 CA 文件名（`gateway.cpp:16-24`）；代码未覆盖其他 loopback 名称。源码调用 libmosquitto 的 peer verification=1；实际 hostname/SNI 的行为由库实现，仓库中 `docs/cloud_mqtt.md` 与 `results/cloud-live-emqx/` 记录了真实 EMQX TLS 连接验证；本地源码没有手写 hostname 比对。旧 `modmqttd` 的包装不同：`libmodmqttsrv/mosquitto.cpp:116-170` 可用 MQTT 3.1.1 或 RPC 模式 MQTT5，`MqttClient::onConnect` 重订阅并 `publishAll`（`mqttclient.cpp:107-132`），其 poll/command 话题和 CAN 云话题不能混同。

云上行 topic：`resume/gateway/devices/{device_id}/telemetry`；下行订阅：`resume/gateway/devices/+/command`；结果：`resume/gateway/devices/{device_id}/command/result`；gateway 状态/心跳：`resume/gateway/status/{client_id}[/heartbeat]`（`gateway.cpp:149-193,299-326`）。云下行 topic 被改写成 `device/{id}/cmd/can_tx` 再用同一 `CommandRouter` 校验。`tests/cloud/live_emqx_test.sh` 与 `tests/cloud/sustained_emqx.py` 对真实 EMQX 做软件 vcan ↔ 云双向验证；100 msg/s 运行有命令 30/30/30，250 msg/s 运行设命令 0（`results/cloud-soak-emqx/{main-100msg-30min,optional-250msg-10min}/final-summary.json`）。结果 schema 仅 `status/detail`，不回显测试输入 `command_id`；测试以每分钟一个 outstanding 命令、CAN 帧序号和时间窗核对（`tests/cloud/sustained_emqx.py:273-391`）。**当前源码未体现真实 CAN 设备 command/result 回执**，只有网关发帧结果。

## 12–14. 生命周期与错误处理清单

| 资源/异常 | 检测与动作；重试/丢弃/指标 | 证据 |
|---|---|---|
| CAN socket/epoll fd | `open` 失败后已有 fd 由 `close` 清理；正常 stop 在 join 后 close；`receive` 对 `epoll_wait<=0` 直接返回 false，未分 EINTR/ERR/HUP；读长度不等于 `can_frame` 则丢并可计 `read_errors`。 | `can_socket.cpp:23-124`、`gateway.cpp:100-124` |
| CAN 发送 EAGAIN/socket error | 单次 `write` 不足整帧即 false；worker 计 `can_errors`、发 `error` status；没有本层重试/errno 分类。 | `can_socket.cpp:74-84`、`gateway.cpp:232-244` |
| RTU 串口 EAGAIN/EINTR/timeout | 独立库 read poll 遇 EINTR 重试；read 遇 EAGAIN/EINTR 返回空；write poll/write 遇 EINTR 或 EAGAIN 重试直到 steady deadline；超时抛异常；`read=0` 未特殊判断开。 | `termios_rtu_transport.cpp:204-256` |
| RTU CRC/半包/噪声 | 候选 CRC 失败继续扫描；无完整帧先保留，超缓冲限时丢旧前缀；解析指标按候选/丢弃统计。 | `termios_rtu_transport.cpp:23-159`、`tests/serial/rtu_transport_tests.cpp:33-68` |
| 旧 Modbus 读写/网络 | libmodbus 返回错误后抛异常，executor 的 retry/watchdog 负责后续；网络失联延时重连，成功后初始重轮询；原服务不公开原始 fd 的 EAGAIN 处理。 | `modbus_context.cpp:115-220`、`modbus_thread.cpp:231-337`、`modbus_executor.cpp`、`modbus_watchdog.cpp` |
| queue full/close | `tryPush=false`、`rejected++`；命令回 `rejected`，遥测记 `canErrors`；stop 唤醒 worker，不保证排空。 | `bounded_queue.hpp:28-76`、`gateway.cpp:187-208,100-124` |
| command timeout | worker 在人工处理延迟后、发送前判期限；`commandTimeouts_++`，不送 CAN，发 timeout status。 | `gateway.cpp:224-244` |
| MQTT 断开/TLS 配置 | callback 清 connected；配置缺 CA/凭据抛异常；库自重连延时 1–30s；重连成功重新订阅；失败的 publish 尝试有限退避，最终计 `publishFailures`。TLS 握手的详细 errno/证书链错误由库处理，当前封装只记录连接结果。 | `gateway.cpp:16-24,60-93,149-175,278-299` |
| MQTT publish API/PUBACK | API 成功计 `published`；可选诊断 MID callback 单列 PUBACK；无应用级持久化重放。 | `gateway.cpp:278-299`、`pipeline_metrics.hpp:70-150` |

旧 Modbus `ModbusContext` 的 `modbus_t*` 在其析构/断连路径释放；`ModbusClient` 的线程由 `stop` join，且主 `ModMqtt::start` 先停这些线程再 shutdown MQTT。具体异常路径与正常路径不同，见第 1 节，不能把正常释放次序推广为所有失败情形。`UnifiedMessage` 入队后由 deque 值持有、出队移给 worker；拒绝则形参/局部对象销毁。`QueueItem` 的 `void*` 经 `getData<T>()` 转成 `unique_ptr<T>`（`queue_item.hpp:11-45`）；读源码时要核对每一分支是否领取，否则不能笼统保证所有路径释放。

## 15. 有证据的试错历史

| 问题/现象 | 发现、根因、修改与最终证据 |
|---|---|
| 基线 CRLF/LF 表象 | `git diff --check` 报大量行尾差异；`git diff --ignore-space-at-eol --stat` 无实质内容变化。保留基线行尾状态并记录验证；`docs/baseline_build.md:195-205`、`results/baseline/line-ending-verification.log`。不能称已修复所有行尾。 |
| ExprTk 缺失 | 原构建成功但直接 Catch2 146/149，`EXPRTK_INCLUDE_DIR-NOTFOUND`；后用临时官方头、标准 Release 构建并补测试注册，原始与修复后证据分别保存；`docs/baseline_build.md:9-21,207`、`docs/implementation_log.md:7-10`。后来云配置回归也记载缺头的 3 例失败（`docs/cloud_mqtt.md`）。 |
| Modbus 异常/CRC 帧 | PTY 模拟器注入 timeout、错误 CRC、短帧；8/8 场景与原始字节见 `tests/modbus/{modbus_rtu_test.sh,rtu_simulator.py}`、`results/functional/modbus/{summary.json,rtu-frames.csv}`。独立解析器的连续坏 CRC 恢复另有 `tests/serial/rtu_transport_tests.cpp:56-68`。两者不应混写为同一实现。 |
| Queue overload | tiny queue + 5ms 人工延迟/1ms deadline 触发 peak 4、641 reject、223 timeout；`tests/integration/reliability_test.sh:19-93`、`results/fault/reliability/{metrics.json,summary.txt}`。改动是否因该测试而来，`docs/implementation_log.md:23-29` 给阶段顺序；当前 Git 历史仅一个 portfolio 快照提交，不能还原每次改动提交。 |
| MQTT 重连/TLS/EMQX | 本地 broker kill/restart、云 broker keepalive 隔离后复连；本地与真实云证据见 `results/fault/reliability/`、`results/cloud-live-emqx/`、`docs/cloud_mqtt.md`。云 100/s、250/s 长时序列账见第 16 节。 |
| ARM64/QEMU | Buildroot 交叉构建与 QEMU virt 验证，早期无界日志耗尽 384 MiB guest 文件系统，随后测试工具直方图零分位错误；保留失败记录、修改测试工具后短回归并完成 8 小时运行。证据 `docs/arm64_buildroot_validation.md`、`results/arm64/soak-failed-nospace-20260921/incident.md`、`results/arm64/soak-histogram-invalid-120s-20260921/`、`results/arm64/soak/`。`

## 16. 性能测试与统计口径对照

| 主张/环境 | 入口与负载 | 成功、P99、CPU/RSS、丢失如何得出；原始证据 |
|---|---|---|
| ARM64/QEMU 1000 msg/s | `tests/load/arm64_stress.cpp`，`scripts/run_buildroot_qemu.py` 调度；vcan 发时间戳帧、独立 MQTT subscriber 收。 | `tests/load/arm64_stress.cpp:140-198,228-325`：发送/接收计数；P99 为 0.1ms 桶的最近秩点，CPU `/proc/<pid>/stat` 的 user+system ticks/测量时长，RSS `/proc/<pid>/status` 的 VmRSS 峰值；loss=`max(sent-received,0)`。历史 30s 稳定记录见 `results/arm64/stress/guest-results/stress-results/index.csv` 与对应档 `result.json`；不同日期 1500 结果相反，不能宣称单一固定上限。 |
| ARM64/QEMU 500 msg/s × 8h | `scripts/run_arm64_soak_8h.sh` 启动 Buildroot/QEMU，guest 使用 `arm64_stress`；运行 28800.000184 秒。 | `results/arm64/soak/guest-results/soak-results/soak-result.json`：14,400,000 发/收、P99 52.7ms、CPU 50.138%、峰值 RSS 3.262MB、loss 0。是模拟 ARM guest 的进程指标，不是 ARM 物理机。 |
| 真实 EMQX 100 msg/s × 30min | `tests/cloud/run_sustained_emqx.sh` → `tests/cloud/sustained_emqx.py:84-325`；`vcan0` 每帧 4-byte run ID + 4-byte 序号，monotonic 定速，独立 TLS subscriber/命令 publisher。 | `tests/cloud/sustained_emqx.py:166-215,350-419` 用 run ID/序号 `seen[]` 对账，另核 gateway API accepted、QoS1 PUBACK、CAN 命令帧和结果；`results/cloud-soak-emqx/main-100msg-30min/final-summary.json` 为 180,000/180,000、缺失 0、重复 0、乱序 64、30 条命令全闭环。**该测试未计算 P99、网关 CPU 或 RSS**。 |
| 真实 EMQX 250 msg/s × 10min | 同脚本不同参数、`commands=0`，只测上行。 | `results/cloud-soak-emqx/optional-250msg-10min/final-summary.json`：150,000 唯一收、150,000 PUBACK、缺失/重复 0、乱序 2；没有下行、P99/CPU/RSS 结论。 |
| 3000 command overload | `tests/integration/reliability_test.sh:65-93` 向 QoS0 broker 输入 3000 行，无逐条 ID。 | `results/fault/reliability/metrics.json` 与 `summary.txt` 仅证 641 拒绝、223 超时、peak 4；“2136 success”没有可核查的逐条成功账，见第 9 节。 |
| x86/WSL2 负载补充 | `tests/load/can_mqtt_load.py:19-189`、`run_load_test.sh`；时间戳 CAN 帧、MQTT 接收回调。 | 此脚本 P99 为排序后线性插值，CPU ticks/墙钟、RSS `VmRSS` 峰值、loss=`sent-received`；`results/summary.csv`、`results/load/*/raw-metrics.json`。与 ARM 直方图 P99 算法不同，不宜直接比较。 |

分段诊断 `src/iot_gateway/pipeline_metrics.hpp:16-154` 的 P99 是指数桶**上界**，并非上述两种端到端 P99；`docs/pipeline_observability.md`、`results/arm64/pipeline/` 讨论 QoS1 publish/PUBACK 及 broker 出站压力。不要把 MQTT publish API 接受、PUBACK 和 subscriber 唯一收到混为一个“成功”。

## 17. 设计取舍：事实与分析分开

| 主题 | 【源码事实】 | 【工程分析与可替代方案】 |
|---|---|---|
| epoll | CAN fd 以 LT/EPOLLIN 注册，接收线程等待（`can_socket.cpp:44-54,86-94`）。 | 适合等待 fd 就绪；当前只有一个 fd，因此收益主要是统一等待接口，替代可为 `poll`/阻塞读。无同条件对照。 |
| non-blocking | CAN socket `SOCK_NONBLOCK`；独立串口 `O_NONBLOCK`（`can_socket.cpp:25`、`termios_rtu_transport.cpp:174`）。 | 避免设备 I/O 无限卡住调用方；需处理短读/短写/EAGAIN。CAN 路径当前处理较简略；替代是有超时的阻塞 I/O。 |
| fixed workers | `max(1,workers)`，共用 queue（`gateway.cpp:93-98`）。 | 控制工作并发；替代是单线程事件驱动、按协议分池、每请求线程。未证明默认 2 最优。 |
| bounded queue | 满即 reject-new、容量默认 1024（`bounded_queue.hpp:28-43`）。 | 限内存和排队增长；替代为阻塞背压、优先级队列、持久化队列。当前命令/遥测互相挤占。 |
| deadline | 从 route 时间戳算超时，在 worker 发送前检查（`command_router.cpp:64-85`、`gateway.cpp:232-245`）。 | 拒绝过期工作；替代为入队前+出队后双检查或设备级确认期限。当前不包含 CAN 执行确认。 |
| QoS1 | 遥测默认 QoS1、状态/命令 QoS1（`gateway.hpp:38`、`gateway.cpp:159,280-289`）。 | 支持 broker ACK；替代 QoS0 减小开销、QoS2 更复杂。`results/arm64/pipeline/` 显示不同 QoS 压力表现，不能以 PUBACK 代替端到端证明。 |
| SocketCAN | PF_CAN RAW 与 vcan0 测试（`can_socket.cpp:25-41`）。 | Linux CAN 软件接口方便模拟与实机接口替换；替代厂商驱动/API。当前物理层未验。 |
| Buildroot | `scripts/build_buildroot_arm64.sh`、`cmake/toolchains/buildroot-aarch64.cmake` 产目标环境。 | 控制交叉编译的系统/依赖；替代 Debian/Yocto 工具链。源码/结果能证明构建，不证明真实板卡行为。 |
| QEMU | `scripts/run_buildroot_qemu.py` 启 ARM64 virt guest。 | 无硬件时验证 ELF、系统与进程；替代真实 ARM 板或容器交叉运行。性能结论仅限所记 QEMU 配置。 |

## 18. 面试问答索引（每题的答案入口）

记号 `S`=源码文件/函数，`T`=测试/结果；单纯构建边界没有运行函数，表中给构建文件。索引中的“无”表示该精确问题没有对应测试，回答时应说 **当前源码未体现 / 无法从仓库确认**，不能编一个测试。文档列在 `T` 栏时仅是旁证，不应冒充测试。先沿入口读代码，再自行口述答案。

### 基础 20 题

| # | 问题 | S / T |
|---:|---|---|
| B01 | 本仓库有几个运行进程？ | `CMakeLists.txt` / `docs/architecture.md` |
| B02 | 新 CAN 进程入口在哪里？ | `src/iot_gateway/main.cpp::main` / `tests/cloud/cloud_config_smoke_test.sh` |
| B03 | 旧 Modbus 进程入口在哪里？ | `modmqttd/main.cpp::main` / `unittests/real_server_tests.cpp` |
| B04 | 配置加载顺序？ | `modmqtt.cpp::init`、`src/iot_gateway/main.cpp::applyCloudEnvironment` / `unittests/server_config_tests.cpp` |
| B05 | 默认 CAN 接口是什么？ | `gateway.hpp::GatewayConfig` / `tests/can/can_mqtt_integration_test.sh` |
| B06 | 谁拥有队列？ | `gateway.hpp::Gateway` / `unittests/iot_gateway_tests.cpp` |
| B07 | `UnifiedMessage` 有哪些字段？ | `unified_message.hpp::UnifiedMessage` / `unittests/iot_gateway_tests.cpp` |
| B08 | CAN ID 如何变设备名？ | `can_socket.cpp::receive` / `results/functional/can/summary.txt` |
| B09 | 遥测发哪个 topic？ | `gateway.cpp::telemetryTopic` / `tests/can/can_mqtt_integration_test.sh` |
| B10 | 云模式发哪个 topic？ | `gateway.cpp::telemetryTopic` / `tests/cloud/live_emqx_test.sh` |
| B11 | MQTT 命令如何识别？ | `command_router.cpp::route` / `unittests/iot_gateway_tests.cpp` |
| B12 | 命令结果意味着什么？ | `gateway.cpp::workerLoop,publishStatus` / `tests/can/can_mqtt_integration_test.sh` |
| B13 | 默认 worker 几个？ | `gateway.hpp::GatewayConfig`、`gateway.cpp::start` / `tests/integration/reliability_test.sh` |
| B14 | 队列满怎么办？ | `bounded_queue.hpp::tryPush` / `results/fault/reliability/metrics.json` |
| B15 | 超时从何时开始？ | `command_router.cpp::route` / `tests/integration/reliability_test.sh` |
| B16 | Modbus RTU 谁处理？ | `modbus_context.cpp::init` / `tests/modbus/modbus_rtu_test.sh` |
| B17 | 串口解析器是否用于生产？ | `src/serial/CMakeLists.txt` / `tests/serial/rtu_transport_tests.cpp` |
| B18 | MQTT 是否有重连？ | `gateway.cpp::start,onConnected` / `results/fault/reliability/summary.txt` |
| B19 | QoS1 如何证明？ | `gateway.cpp::publish`、`pipeline_metrics.hpp::PublishTracker` / `results/cloud-soak-emqx/main-100msg-30min/final-summary.json` |
| B20 | 真实 CAN 硬件测过吗？ | `can_socket.cpp::open` / `docs/PROVENANCE.md`（物理层：无） |

### 源码 20 题

| # | 问题 | S / T |
|---:|---|---|
| C01 | epoll fd 在哪里创建/关闭？ | `can_socket.cpp::open,close` / `tests/can/can_mqtt_integration_test.sh` |
| C02 | 为什么是 LT？ | `can_socket.cpp::open` 的 `EPOLLIN` / 无 ET 对照测试 |
| C03 | EPOLLERR/HUP 如何处理？ | `can_socket.cpp::receive`（无分支）/ 无 |
| C04 | 一次 epoll 唤醒读几帧？ | `can_socket.cpp::receive` / `tests/load/can_mqtt_load.py` |
| C05 | read EAGAIN 怎么办？ | `can_socket.cpp::receive`（不分类）/ 无 |
| C06 | CAN error/RTR 怎样隔离？ | `can_socket.cpp::receive` / `tests/can/send_invalid_can.py` |
| C07 | CAN 发送失败如何上报？ | `can_socket.cpp::send`、`gateway.cpp::workerLoop` / `unittests/iot_gateway_tests.cpp` |
| C08 | RTU 串口是哪种参数？ | `termios_rtu_transport.cpp::open` / `tests/serial/rtu_transport_tests.cpp` |
| C09 | RTU 半包存在哪里？ | `termios_rtu_transport.cpp::feed` 的 `buffer_` / `tests/serial/rtu_transport_tests.cpp::parserScenarios` |
| C10 | 粘包如何拆？ | `termios_rtu_transport.cpp::feed` 的 while / 同上 |
| C11 | CRC 错误立即丢帧吗？ | `termios_rtu_transport.cpp::feed,validCrc` / 同上 |
| C12 | Modbus 轮询谁调度？ | `modbus_thread.cpp::run`、`modbus_scheduler.cpp::getRegistersToPoll` / `unittests/scheduler_tests.cpp` |
| C13 | Modbus 写指令经过哪条队列？ | `mqttclient.cpp::onMessage`、`modbus_client.hpp::sendCommand` / `unittests/mqtt_command_tests.cpp` |
| C14 | 消息 type erasure 怎么领取所有权？ | `queue_item.hpp::getData` / `unittests/modbus_request_queues_tests.cpp` |
| C15 | worker 怎样退出？ | `bounded_queue.hpp::stop,pop`、`gateway.cpp::workerLoop,stop` / `unittests/iot_gateway_tests.cpp` |
| C16 | 发布 API 成功和 PUBACK 差别？ | `gateway.cpp::publish`、`pipeline_metrics.hpp::submit,completed` / `unittests/pipeline_metrics_tests.cpp` |
| C17 | 早到的 publish callback 如何关联？ | `pipeline_metrics.hpp::PublishTracker` / `unittests/pipeline_metrics_tests.cpp` |
| C18 | 云 topic 如何回到统一 router？ | `gateway.cpp::onMessage` / `tests/cloud/live_emqx_test.sh` |
| C19 | 旧服务 RPC 与 poll 如何区别？ | `modbus_types.hpp::ModbusMessageBase`、`mqttclient.cpp::handleRpcRequest` / `unittests/rpc_tests.cpp` |
| C20 | 新 CAN 与旧 Modbus 共用数据模型吗？ | `unified_message.hpp`、`modbus_messages.hpp` / `docs/architecture.md` |

### 设计 15 题

| # | 问题 | S / T |
|---:|---|---|
| D01 | 为什么拆两进程？ | `CMakeLists.txt` / `docs/architecture.md` |
| D02 | epoll 对单 fd 有什么收益/成本？ | `can_socket.cpp::receive` / 无对照实验 |
| D03 | 不读到 EAGAIN 有什么影响？ | `can_socket.cpp::receive` / `results/arm64/pipeline/` |
| D04 | 为什么 worker 固定？ | `gateway.cpp::start` / `docs/scalability_architecture_review.md` |
| D05 | 遥测与命令共队列的代价？ | `gateway.hpp::queue_` / `results/fault/reliability/` |
| D06 | reject-new 与阻塞入队怎样取舍？ | `bounded_queue.hpp::tryPush` / `results/fault/reliability/metrics.json` |
| D07 | 为什么用 steady_clock 判超时？ | `command_router.cpp::route`、`gateway.cpp::workerLoop` / `tests/integration/reliability_test.sh` |
| D08 | QoS1 到哪里算成功？ | `pipeline_metrics.hpp::PublishTracker` / `tests/cloud/sustained_emqx.py` |
| D09 | 能否用 QoS0 提吞吐？ | `gateway.cpp::publish` / `docs/pipeline_observability.md` |
| D10 | 为什么 vcan 不等于物理 CAN？ | `can_socket.cpp::open` / `docs/arm64_buildroot_validation.md` |
| D11 | 29 位 CAN ID 发送有什么风险？ | `command_router.cpp::route`、`can_socket.cpp::send` / 无扩展帧覆盖证据 |
| D12 | Buildroot 的作用？ | `scripts/build_buildroot_arm64.sh` / `results/arm64/buildroot/` |
| D13 | QEMU 结果可外推实机吗？ | `scripts/run_buildroot_qemu.py` / `results/arm64/soak/`（不可直接外推） |
| D14 | 独立 RTU parser 为何没改变原服务？ | `src/serial/CMakeLists.txt`、`modbus_context.cpp::init` / `tests/serial/rtu_transport_tests.cpp` |
| D15 | 替代旧 SPSC 无界队列的方案？ | `modbus_client.hpp::sendCommand` TODO / 无实测替代方案 |

### 故障定位 10 题

| # | 问题 | S / T |
|---:|---|---|
| F01 | broker 断开时怎样确认重订阅？ | `gateway.cpp::onDisconnected,onConnected` / `tests/integration/reliability_test.sh` |
| F02 | TLS CA 配错在哪失败？ | `gateway.cpp::Gateway,start` / `tests/cloud/cloud_tls_local_test.sh` |
| F03 | CAN 接口不存在在哪失败？ | `can_socket.cpp::open` / 无专门断言该失败的测试 |
| F04 | queue_rejected 上升代表什么？ | `bounded_queue.hpp::tryPush` / `results/fault/reliability/metrics.json` |
| F05 | command_timeouts 上升在哪查？ | `gateway.cpp::workerLoop` / 同上 |
| F06 | can_errors 一定是总线坏了吗？ | `gateway.cpp::receiveLoop,workerLoop` / `tests/can/send_invalid_can.py` |
| F07 | CRC 错误后为何暂不出帧？ | `termios_rtu_transport.cpp::feed` / `tests/serial/rtu_transport_tests.cpp` |
| F08 | Modbus 长时间失败如何重连？ | `modbus_thread.cpp::run`、`modbus_watchdog.cpp` / `unittests/modbus_watchdog_tests.cpp` |
| F09 | 启动失败资源是否都回收？ | `gateway.cpp::start,stop,~Gateway` / 无统一异常回滚测试 |
| F10 | publish 成功但订阅者少消息怎么定位？ | `pipeline_metrics.hpp::PublishTracker` / `results/arm64/pipeline/` |

### 性能测试 10 题

| # | 问题 | S / T |
|---:|---|---|
| P01 | x86 负载怎么发 CAN 帧？ | `tests/load/can_mqtt_load.py::main` / `results/load/` |
| P02 | ARM 1000/s 的判稳条件？ | `tests/load/arm64_stress.cpp::main` / `results/arm64/stress/guest-results/stress-results/index.csv` |
| P03 | ARM P99 怎么算？ | `tests/load/arm64_stress.cpp::Subscriber::percentile` / `results/arm64/soak/guest-results/soak-results/soak-result.json` |
| P04 | x86 P99 怎么算？ | `tests/load/can_mqtt_load.py::percentile` / `results/summary.csv` |
| P05 | 8h 1440 万怎样对账？ | `tests/load/arm64_stress.cpp::main` / `results/arm64/soak/guest-results/soak-results/soak-result.json` |
| P06 | EMQX 18 万怎样去重？ | `tests/cloud/sustained_emqx.py::on_message` / `results/cloud-soak-emqx/main-100msg-30min/final-summary.json` |
| P07 | 250/s 云测试测了下行吗？ | `tests/cloud/sustained_emqx.py::main` / `results/cloud-soak-emqx/optional-250msg-10min/final-summary.json` |
| P08 | CPU/RSS 取哪个进程？ | `tests/load/arm64_stress.cpp::readProcess` / `docs/resume_evidence.md` |
| P09 | 3000 命令能得多少 success？ | `tests/integration/reliability_test.sh` / `results/fault/reliability/metrics.json`（无法确认） |
| P10 | 5000/s burst 是成功档吗？ | `tests/load/can_mqtt_load.py::main` / `docs/benchmark.md`、`results/summary.csv`（否） |

## 19. 初学者源码阅读顺序

1. **边界与来源**：`CMakeLists.txt`、`docs/PROVENANCE.md`、`docs/architecture.md`。先确认两个进程和独立串口库。
2. **CAN 入口与配置**：`src/iot_gateway/main.cpp::main,applyCloudEnvironment` → `gateway.hpp::GatewayConfig`，回答参数优先级与默认值。
3. **资源启动**：`gateway.cpp::Gateway,start,stop`，逐个标记成员、fd、线程、正常/异常清理。
4. **CAN I/O**：`can_socket.cpp::open,receive,send,close`，手画 epoll fd、CAN socket、一次读和一次写。
5. **统一模型与路由**：`unified_message.hpp/.cpp::UnifiedMessage,toJson` → `command_router.cpp::route`，用一个上行帧与一个下行 JSON 做演练。
6. **队列与 worker**：`bounded_queue.hpp::tryPush,pop,stop` → `gateway.cpp::receiveLoop,onMessage,workerLoop`，画出两个生产者和 N 个消费者、拒绝与超时分支。
7. **MQTT/云**：`gateway.cpp::onConnected,onDisconnected,publish,telemetryTopic,publishStatus` → `pipeline_metrics.hpp::PublishTracker`，区别 API 接受/PUBACK/订阅端交付。
8. **CAN 闭环测试**：`tests/can/can_mqtt_integration_test.sh` → `tests/cloud/live_emqx_test.sh`、`tests/cloud/sustained_emqx.py` → `results/cloud-soak-emqx/.../final-summary.json`，能完整复述 CAN→gateway→MQTT→EMQX 与逆向 command/result。
9. **旧 Modbus 入口**：`modmqttd/main.cpp::main` → `libmodmqttsrv/modmqtt.cpp::ModMqtt::init,start` → `config.cpp`；不把旧线程塞进 CAN 进程。
10. **Modbus 线程与 I/O**：`modbus_client.cpp::start,stop` → `modbus_thread.cpp::run,dispatchMessages` → `modbus_scheduler.cpp`、`modbus_executor.cpp`、`modbus_context.cpp`，再看 `mqttclient.cpp::onMessage,processRegisterValues,publishState`。
11. **RTU 字节流实验**：`src/serial/termios_rtu_transport.cpp::open,readFrames,feed` → `tests/serial/rtu_transport_tests.cpp`；与 `tests/modbus/rtu_simulator.py`、`modbus_context.cpp` 的 libmodbus 路径分开讲。
12. **性能与边界**：`tests/load/arm64_stress.cpp`、`tests/load/can_mqtt_load.py`、`tests/cloud/sustained_emqx.py` → 对应 `results/`；最后读 `docs/scalability_architecture_review.md`，练习把观测、源码事实、工程推断分开陈述。

复习时每条路径都问四件事：谁拥有对象，在哪个线程执行，失败时数据去哪里，哪个测试/结果真正观察了它。答不到证据时使用“**当前源码未体现 / 无法从仓库确认**”。

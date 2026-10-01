# 网关架构与本地验收
更新：2026-10-01。按用户最新范围，本轮以本地验证收尾，云端通信不纳入验收。
基线 HEAD e91a31762c07aebe50c448964c8a2d5af2ebe52d；本轮变更尚未提交。
证据根目录：results/resume_alignment/20260930T143425Z/。

## 问题与改造
原先 mqmgateway_iot 的运行路径以 CAN 为主，Termios RTU 仅有独立传输/解析测试。
现在 Gateway::start 创建 SouthboundReactor，同一接收线程的一个 epoll 同时监听
CAN 原始套接字、RTU 串口和跨线程 eventfd。两路输入生成 UnifiedMessage，
进入同一个容量受限队列，再由固定 workers 处理并发布 MQTT。
这是对独立传输模块的实际整合；旧 modmqttd/libmodmqttsrv 保留原有架构，不把它混称为新 reactor。

线程关系：主线程负责启动/信号/回收；1 个 southbound 接收线程；2 个 worker；
1 个 heartbeat 线程；1 个 libmosquitto 网络线程，正常总计 6 个。
CAN 对象保留兼容 receive() 使用的内部 epoll；新接收线程实际等待的是
SouthboundReactor 的共享 epoll，不串行等待两个独立事件循环。
混合测试日志记录同一 PID=910980、shared epoll=5、CAN fd=3、serial fd=7。
这些 fd 数值是该次运行快照，不是硬编码配置。

数据流：
- RTU：定时发功能码 03 的单寄存器请求 → PTY/串口响应 → CRC/流式重组 →
  UnifiedMessage → BoundedQueue → worker → MQTT telemetry。
- CAN：SocketCAN 接收 → UnifiedMessage → 同一个队列/worker → MQTT telemetry。
- MQTT CAN 下行：CommandRouter → 同一工作队列 → worker 调用 CAN send → status。
- MQTT RTU 下行：CommandRouter → 同一工作队列 → worker 提交有界串行事务队列 →
  eventfd 唤醒接收线程 → 功能码 06 写单寄存器 → 校验回显后才发布成功状态。
RTU fd 只由接收线程读写；CAN socket 允许独立接收线程和发送 worker 使用。
一次只保留一个 RTU 在途事务，命令过期、CRC 错误、串口关闭都有明确处理。

## 参数及取舍
| 参数 | 当前值 | 意义与增减影响 |
|---|---|---|
| queue/workers | 1024 / 2 | 有界突发缓存和固定并发；扩大队列增加内存与排队延迟，增加 worker 可能增加竞争 |
| RTU | 115200、8N1、slave=1、register=0 | 当前一个串口一个配置从站；不是多从站总线调度器 |
| poll/response | 100 ms / 1000 ms | 约 10 次读取/秒；缩短轮询增加总线负载，缩短超时可能误判慢设备 |
| RTU parser buffer | 1024 B | 控制噪声缓存；清理缓冲时保留累计错误计数 |
| reactor 单轮预算 | CAN 64 帧、RTU 16 次 512 B 读取 | 限制单路占用；LT 就绪会再次触发，确保另一协议和超时能推进 |
| 事务间静默 | 2 ms | 115200 下采用保守间隔；低波特率/真实 RS-485 时序仍需专门验证 |
| MQTT | QoS1，inflight 20 | QoS1 是 broker 确认，不等同端到端恰好一次 |
| 故障参数 | queue=4、workers=1、处理延迟=5 ms、命令期限=1 ms | 故意造成排队拒绝/超时，仅用于故障回归 |

## 本轮真实结果
普通 30 秒混合测试：CAN 3000/3000、RTU 293/293，重复/缺失均 0，
两种下行均确认，P50/P95/P99=0.564/0.942/1.139 ms，
网关进程平均 CPU=3.08%（单核 100%）、峰值 RSS=5956 KiB。
测量起点为模拟器发出 CAN / PTY 响应，终点为独立 MQTT 观察者回调；
PTY/vcan 不包含物理总线耗时。

最终故障混合测试 mixed-final-faults：30 秒，CAN 3000/3000、RTU 273/273，
重复/缺失 0，每个完整 5 秒窗口两路都推进，下行均成功。
主动注入坏 CRC/噪声/一次无响应，最终保留 10 个 CRC 候选拒绝、10 字节丢弃及超时记录。
不能把“CRC 候选数”说成准确坏帧数。
lifecycle-final 首次回归因测试没有启动 broker，先触发连接拒绝；原日志保留。
补齐本地 broker 的 lifecycle-final-retry 通过：错误串口启动返回 1，
运行中串口断开返回 1，不再因接收线程异常直接终止或空转。

reliability-final：broker 恢复 1.523383 秒，拒绝 578，命令超时 228，
发布失败 0，队列峰值 4。这里拒绝/超时是故障场景预期，不是正常负载丢包。
全量原生 CTest 3/3 通过（unit_tests 含多项 Catch 测试，不等于仅三条断言）。
最终小修后针对性 CTest 2/2 通过。
最终 ARM64 交叉构建和 QEMU 本地集成通过，QEMU 主机时长 20.892 秒。
qemu-final-local/injected-binaries.json 保存被注入新镜像的二进制散列。
QEMU 回归覆盖 RTU 传输单测、CAN/MQTT 双向、非法命令、broker 重连；
同进程双协议证明来自原生 PTY/vcan 混合测试，未声称 QEMU 中也做了混合协议证明。

此前本轮短阶梯为每档 10 秒：100..3000 msg/s STABLE；5000 FAIL；
4000 两次 FAIL。这是短测试档位结果，不是绝对容量上限，
且早于最终统计/退出处理修补；按最新要求不再追加长压测。
保存于 qemu-short-ladder/guest-results/stress-results/。

## 实际问题、边界和下一步
已修正 RTU 未接入公共 reactor、扩展 CAN ID 标志、RTU 状态确认、
超时清理丢失累计统计、RTU 队列拒绝被错误算成 CAN 错误、
启动失败资源回收、串口失效退出。
当前 RTT/资源仅代表 WSL/QEMU 软件环境，没有物理 CAN/RS-485 验证。
持续 MQTT 离线没有持久化磁盘 spool，不能承诺离线不丢；
长时间下行命令流可能推迟周期轮询，后续可做轮询/命令加权调度。
下一轮才考虑多个 RTU 从站、请求关联、持久化和更细的优先级调度。

格式目标 format-check/tidy-naming 在当前配置中不存在，尝试失败已保存 lint.log。
仓库原有大量 CRLF 差异及尾随空白，原始 git diff --check 返回 2；
未通过整仓行尾转换掩盖结果。最终审计列出原始检查结果与变更源码散列。

## 简历证据矩阵
| RESUME CLAIM | SOURCE FILES/FUNCTIONS | TEST | RESULT | STATUS |
|---|---|---|---|---|
| 同进程 RTU+CAN reactor | src/iot_gateway/southbound_reactor.cpp:run/tick | mixed-final-faults | 两路同时推进、零缺失 | VERIFIED |
| 统一消息/有界队列/固定线程池 | unified_message.hpp、bounded_queue.hpp、gateway.cpp:workerLoop | mixed / reliability-final | 正常传输与拒绝边界通过 | VERIFIED |
| MQTT 双协议上下行 | command_router.cpp、gateway.cpp、SouthboundReactor::received | mixed-final-faults | 两种命令确认成功 | VERIFIED |
| RTU 流式 CRC/分片/超时恢复 | src/serial/termios_rtu_transport.cpp | CTest / mixed-final-faults | 通过，保留错误统计 | VERIFIED |
| ARM64 运行 | toolchain、scripts/run_buildroot_qemu.py | qemu-final-local | 新二进制集成通过 | VERIFIED |
| 物理总线时序 | 当前软件实现 | 无硬件测试 | 尚未验证 | IMPLEMENTED_NOT_FULLY_VALIDATED |
| 本轮云端通信 | 原有 MQTT 配置路径 | 用户已取消 | 不纳入本轮验收 | IMPLEMENTED_NOT_FULLY_VALIDATED |

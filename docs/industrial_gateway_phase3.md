# Phase 3：Modbus TCP 与 Generic TCP

日期：2026-10-02。基于 Phase 1–2 未提交工作区继续实现。历史结果保留在原目录，本轮证据为 results/edge_core/phase3-20261002/。

## 实际结构

每个配置的 TCP 设备各有一只驱动、一条非阻塞连接和独立状态；共享 SouthboundReactor、GatewayCore、现有有界 worker 队列和 MQTT 发布路径。Gateway 没有新增协议分支。通用 TcpTransport 只负责非阻塞 IPv4 连接、读写、关闭；两种驱动各自负责编码/验帧。

Reactor 现在可在 tick 后增删 FD 并更新事件兴趣。每次注册分配新 token，丢弃旧连接遗留的 epoll 事件，避免 FD 数字重用误投递。断线采用单调时间退避，事件循环不执行阻塞 DNS（当前仅允许数值 IPv4）。同一 Reactor 还继续承载 CAN 和 RTU。

Modbus TCP 使用 MBAP 长度、事务号、从站号与功能码校验，周期性读一个保持寄存器，f06 写单寄存器，并处理设备异常响应。每设备一笔请求在途；半包、粘包通过 MBAP 长度还原。写请求只有收到相同事务的 f06 回显才标记 `ok`。断线后的未知写入不重放，以免设备重复执行。

Generic TCP 使用明确的线缆格式：2 字节无符号大端长度，后接 1–4096 字节不透明载荷。它不臆测 PLC/自定义协议语义。收到完整帧就发布遥测；`tcp_send` 的 `ok` 表示已写入本机 TCP socket，**不是设备应用层确认**。如设备需要业务确认，应另定义协议驱动。

## 配置与消息

可重复传入：

```text
--modbus-tcp=id,IPv4,port[,slave,register,poll_ms,response_ms,reconnect_ms]
--generic-tcp=id,IPv4,port[,slave,register,poll_ms,response_ms,reconnect_ms]
```

例子：

```text
--modbus-tcp=plc,127.0.0.1,1502,1,0,100,500,100
--generic-tcp=sensor,127.0.0.1,9000
```

设备 ID 限字母、数字、下划线、连字符；重复 ID 启动时拒绝。端口、从站、寄存器、正整数时间和有界队列在构造时验证。默认无 TCP 设备，Phase 2 行为保持。

MQTT 命令：

```text
device/plc/cmd/modbus_tcp_write  {"slave":1,"register":7,"value":1234}
device/sensor/cmd/tcp_send       {"data":"0405"}
```

通用 TCP 长度和十六进制 payload 在 CommandRouter 兼容边界校验；设备/驱动归属由 GatewayCore 和 DriverManager 校验。云模式的默认命令通过驱动注册信息解析。

指标增加每 TCP 设备的 `connections`、`disconnects`、`timeouts`、`malformed`、`rejected`；键名如 `modbus_tcp:plc`、`generic_tcp:sensor`。

## 实测验收

| 项目 | 结果 |
| --- | --- |
| 本地构建 | PASS，Release/Ninja |
| 完整 CTest | 5/5 PASS，170.15 秒（当时版本）；最终新增写入不重放断言后 TCP 单项另行复验 |
| 两 TCP 设备运行探针 | PASS：半包/粘包、读、写、事务号错、非法长度、超时、重连 |
| MQTT 完整链路 | PASS：两设备遥测上行；PLC f06 与 Generic TCP 下行及状态；非法命令拒绝 |
| 旧 CAN/RTU 混合回归 | PASS：CAN 1000/1000，RTU 102/102，零丢失/重复，双命令成功 |
| ARM64/QEMU | 两 TCP 设备探针、既有 RTU 探针和旧 CAN/RTU/MQTT 烟测均 PASS；最终版本见 qemu-final/ |

最终 TCP 探针还验证：已发出的 f06 写在回显前断线，状态为 `unavailable`，重连后首笔仍是读请求而非旧写重放。ARM64 探针是 QEMU 客户机内的 loopback 模拟设备；MQTT 端到端测试在 WSL 本地运行，不能合称为 ARM64 云端闭环。

完整原始日志：build.log、build-arm64.log、ctest.log、tcp-test-final.log、mqtt-e2e.log、mixed-run.log、qemu-final-run.log；结构化结果在 mqtt-e2e/summary.json、qemu-final/guest-results/results/tcp-driver-probe.json 与其他 guest 结果中。lint.log 显示本机 CMake 没有 format-check/tidy-naming 目标，没有标为通过。没有修改 modmqttd/libmodmqttsrv、docs/original_architecture.md 或旧 results。

## 当前限制

仅实现数值 IPv4 客户端；未实现 DNS、IPv6、TCP TLS 或复杂 Modbus 批量读写/多寄存器。Generic TCP 的 2 字节长度是本项目定义的测试设备协议，真实设备必须使用相同格式或另写驱动。尚未在真实 PLC/硬件上验证。本轮未做长期压力或性能上限测试。

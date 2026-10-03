# Phase 5：工业协议驱动（2026-10-02—03）

基线 HEAD：`644187153744cf7ecb66ccdca9f9be44ba87e8f5`。Phase 1–4 的未提交工作和历史证据原样保留；本阶段未提交、未推送。依赖与许可审计见 [Phase 5A](industrial_gateway_phase5_dependency_audit.md)，原始日志和结构化结果见 [Phase 5 证据](../results/edge_core/phase5-20261002/)。

## 实施范围和状态

| 协议 | 状态 | 本次可验证的协议子集 |
| --- | --- | --- |
| Mitsubishi MC | VERIFIED_SOFTWARE | TCP、MC 3E **binary** 帧、CPU 路由、D 区一个 16 位 word 的 `0401` 读取和 `1401` 写入；地址为显式十进制 D 地址。 |
| OPC UA | VERIFIED_SOFTWARE | open62541 1.3.15 client；可配置 endpoint/NodeId；定时 Read、可写点 Write；Boolean、非负 Int32/UInt32、String；保留 NodeId、StatusCode、来源/服务器时间戳。使用 polling，未实现订阅。 |
| Siemens S7 | VERIFIED_SOFTWARE | 官方 Snap7 client；S7 TCP 连接到可配置地址、rack/slot/port；DB 区一个 16 位 word 的 DBRead/DBWrite；点位来源保留 `DBn.DBWoffset`。 |
| ADC / PWM | NOT_IMPLEMENTED | 仍按 Phase 4 计划延后，未引入占位驱动。 |

三种协议都在 `src/drivers/` 中有真实实现、独立设备 ID/点位、错误与重连计数。OPC UA 与 S7 由 `WITH_OPCUA` / `WITH_S7` 控制，未启用时基础构建不需要相应第三方库。MC 使用现有非阻塞 TCP transport 和 Reactor；OPC UA/S7 的同步库调用交给有界的共享采集调度器，不为每个点位创建常驻线程。当前调度器固定 2 条 worker；如果多个库调用同时长时间阻塞，采集延迟会增加，不宣称硬实时。

生产上行路径：`driver → DeviceRegistry → PointRegistry/PointMapper → UnifiedMessageV2 → BoundedQueue/worker → MQTT`。下行路径：`MQTT → CommandRouter → GatewayCore.prepare → 命令队列 → GatewayCore.submit → DriverManager → driver → status`。`GatewayCore` 不按协议名或设备 ID 前缀分支。写入成功以协议确认/库返回为准；MC 写出后断线而未收到应答时标为 `unknown-result`，不自动重放。OPC UA/S7 写失败保留协议错误信息，不把发送动作直接视为执行成功。

## 配置格式

```text
--mc=mc01,speed,127.0.0.1,5000,7,50,300,50
--opcua=opcua01,answer,opc.tcp://127.0.0.1:4840,ns=1;s=the.answer,integer,50,500,1
--s7=s701,db_word,127.0.0.1,0,2,1,0,50,1,102
```

MC 字段：`device_id,point_id,IPv4,port,D_address[,poll_ms,response_ms,reconnect_ms]`。OPC UA 字段：`device_id,point_id,endpoint,NodeId,type,poll_ms,response_ms,writable`，`type` 为 `integer|boolean|text`。S7 字段：`device_id,point_id,IPv4,rack,slot,DB_number,byte_offset,poll_ms,writable[,port]`，默认端口 102。重复 device ID 在注册时拒绝；每个配置实例有独立驱动和状态计数。示例中的本地地址是测试地址，不代表已连接工业设备。

## 运行验证

- 本机 Release 全量构建成功；启用 ExprTk、OPC UA、S7 的完整 CTest **11/11 PASS**。日志：`native-full/configure-final.log`、`build-final.log`、`ctest-final.log`。其中原有 `unit_tests` 170.30 s，新增 `phase5_full_mixed_tests` 0.93 s。CAN、RTU、Modbus TCP、Generic TCP、Phase 4 板级/串口软件组成及三个新驱动运行测试均包含在此次 CTest。
- 同时验证 `WITH_OPCUA=OFF`、`WITH_S7=OFF` 的基础 `mqmgateway_iot` Release 构建通过；日志在 `native-base-options-off/`，可选依赖未破坏基础构建。
- MC：确定性本地 MC 3E 服务端检验真实帧、拆包/粘包、读写、错误码、格式错误、超时、断线恢复和两个设备；本地 Mosquitto E2E 检验 `device/mc01/telemetry` 的映射值 4660、写命令 `accepted/success` 及非法地址拒绝。结果：`local-mc-mqtt-final/summary.json` PASS。
- OPC UA：真实 open62541 软件服务器和客户端验证连接、两个有效节点/设备、坏 NodeId、坏 StatusCode、时间戳、读写、断线恢复、点位与命令映射。ARM64 客体结果：`arm64-opcua-enhanced/guest-results/results/opcua-driver-probe.json` PASS。
- S7：真实 Snap7 `TS7Server` 与 client 验证两个 DB 设备、错误 DB、读写、断线恢复、来源地址、点位与命令映射。ARM64 客体结果：`arm64-s7-enhanced/guest-results/results/s7-driver-probe.json` PASS。
- 同一个 GatewayCore 的 `MIXED_SOFTWARE_INTEGRATION` 同时运行 CAN、Modbus RTU、Modbus TCP、Generic TCP、SPI/I2C/GPIO 测试后端、Raw UART、MC、OPC UA、S7，合计 **11 个设备**；读、写、点位映射、慢 SPI 时 I2C 继续采集、UART 故障与重连均由测试检查。最终 ARM64 客体 `arm64-full-mixed-final/guest-results/results/phase5-full-mixed-probe.json` 为 PASS：42 条驱动消息、0 丢弃、0 映射失败、6 次命令提交、0 拒绝、测试进程峰值 RSS 5076 KiB、累计 CPU 165 ms；QEMU 8.2.2 主机运行 34.938 s。这里的 CPU 是该短测试进程累计时间，**不是**长期网关 CPU 占用率；SPI/I2C/GPIO 是测试后端。该组成测试直接接收 GatewayCore 回调，不单独验证 MQTT 队列。
- 实际网关进程 + 本地 MQTT + vcan0/PTY 的 10 秒 CAN/RTU 回归：CAN 1000/1000、RTU 103/103、0 缺失、0 重复、两种命令确认、无协议饥饿；队列峰值 1、拒绝 0、MQTT 发布失败 0。证据：`mixed-can-rtu-final/summary.json`。这只是短回归，不是 Phase 5 性能极限。
- ARM64 使用 Buildroot 交叉编译并把新二进制和 Snap7 动态库注入**私有临时镜像**。`arm64-full-mixed-final/` 保存编译日志、注入二进制 SHA-256 清单、QEMU 控制台、客体结果和主机耗时；基础 ARM 测试与 11 设备探针均 PASS。open62541 为 ARM64 静态链接，Snap7 为 ARM64 动态链接。历史 Buildroot 镜像和 `results/arm64/` 未被改写。

## 调试记录与局限

首次本机完整 CTest 有 3 个旧用例因可选 ExprTk 未接入而找不到 `exprconv.so`；原始失败日志保留在 `native-full/ctest.log`。找到已有的 `/tmp/exprtk-src/exprtk.hpp`（SHA-256 `a056126bb009b5676a0aade83f86035d384ea8fd4392037380e4774353871218`），仅重配构建依赖后 11/11 通过，未放宽测试或修改旧业务逻辑。S7 初次 ARM 注入因动态库文件名不符而不能启动，失败证据保留于 `arm64-s7-qemu-rerun/`；以 ELF 所需文件名注入后 `arm64-s7-enhanced/` PASS。OPC UA NodeId 解析曾为每次轮询分配未释放的输入缓冲；移除该多余分配，重新编译并通过本机及 ARM64 混合回归。

当前三个 Phase 5 驱动都是一个配置实例对应一个设备点位；同一物理端点下多个点位要配置为多个逻辑设备/连接，尚未实现会话复用或一个 device ID 下的多点采集。MC 未实现 M 位、ASCII/4E 帧或任意批量请求；S7 未实现 M/I/Q、其他 PLC 家族矩阵或安全认证；OPC UA 未实现订阅、证书/加密配置、复杂数组/结构体，且当前整数映射不接收负数。软件服务器与 QEMU 结果不等于实体 PLC/现场总线或安全模式验收。Phase 6 的告警、最终健康状态策略、诊断管理和看门狗仍未开始。

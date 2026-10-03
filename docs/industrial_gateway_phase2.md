# Phase 2：CAN / Modbus RTU 驱动迁移

日期：2026-10-02。起始 HEAD：644187153744cf7ecb66ccdca9f9be44ba87e8f5。
基于 Phase 1 未提交工作继续实施；此 HEAD 不代表本轮工作区源码快照。

## 实际运行路径

上行：SocketCAN / RTU transport → 共享 SouthboundReactor → 驱动回调 → GatewayCore → V2 有界队列 → worker → MQTT。

下行：MQTT → CommandRouter 兼容校验 → GatewayCore.prepare → V2 有界队列 → worker → GatewayCore.submit → DriverManager → 驱动。
驱动的状态回执同样经过 GatewayCore；外部 topic、JSON 和状态文本保持兼容。

Gateway 不再按协议选择发送路径。Reactor 仅处理通用 FD、就绪回调、tick 和唤醒。
CAN 迁移完成后先执行独立 CAN 验证，再迁移 RTU，随后执行混合回归。

## 文件与职责

- src/drivers/can_driver.*：SocketCAN 接收、发送、状态回执与指标。
- src/drivers/modbus_rtu_driver.*：串口轮询、单事务在途、命令队列、CRC、超时及恢复。
- src/drivers/register_drivers.*：集中注册两种驱动。
- src/edge_core/：设备归属、驱动生命周期、V2 校验/派发和统一消息入口。
- src/iot_gateway/gateway.*：去掉协议分支、直接 CAN/RTU 操作；使用 V2 队列和 GatewayCore。
- src/iot_gateway/southbound_reactor.*：删除旧 RTU 状态机，使用通用事件驱动接口。
- src/iot_gateway/can_socket.*：删除已无调用者的独立 receive/epoll 路径，保留底层传输。
- CommandRouter 保留唯一一套旧命令报文解析，属于外部兼容边界，不是第二套运行时驱动。
- 新增 driver_framework_tests.cpp、rtu_driver_runtime_tests.cpp；QEMU 脚本新增可选 --rtu-driver-probe。

CAN 驱动保留历史设备别名（排除 rtu- 命名空间）；RTU 接受配置的从站 ID。归属规则位于驱动而非核心，歧义拒绝执行。动态设备注册上限 65536；后续多驱动建议使用显式绑定。
驱动生命周期由网关控制线程串行调用；提交期间有管理器锁保护，启动失败回滚并清理回调。
复用 TermiosRtuTransport，不新增平行串口封装。设备/点位配置加载、采集调度、其他协议、健康告警是后续阶段。

## 验证证据

全部保存于 results/edge_core/phase2-20261002/，历史目录未覆盖。

| 验证 | 实测结果 |
| --- | --- |
| CAN 迁移独立检查点 | 4/4：上行、下行、非法命令、非法 CAN |
| 新核心/驱动单元测试 | 8 用例、92 断言通过 |
| 原有完整 CTest | 3/3 通过，170.14 秒 |
| 新 RTU 驱动 CTest | 单独 1/1 通过 |
| 正常混合 10 秒 | CAN 1000/1000，RTU 102/102，零丢失/重复，双协议命令成功 |
| 故障混合 30 秒 | CAN 3000/3000，RTU 274/274，零丢失/重复，无饥饿 |
| 故障混合延迟 | P50 0.5281 / P95 0.8909 / P99 1.1005 ms |
| 故障混合资源 | WSL 网关 PID 平均 CPU 2.8471%（单核口径），峰值 RSS 6048 KiB，队列峰值 2 |
| 生命周期 | 无效串口启动和串口断开均报告失败并退出，无信号异常终止 |
| 背压/重连 | 恢复成功；注入导致 rejected=355、command_timeouts=229 |
| 本地 TLS | 证书校验、上下行、命令回执、重连及重连后传输通过 |
| ARM64/QEMU 原有烟测 | RTU 字节流、CAN 双向、非法命令、broker 重连通过；短负载 500/500 |
| ARM64/QEMU 新驱动探针 | 真实 PTY → RTU 驱动 → Reactor → GatewayCore；读、写确认、坏 CRC、超时及恢复通过 |

最终 QEMU 结果为 qemu-final/，主机执行用时 21.269 秒；先前 qemu/ 和 qemu-driver/ 均保留。
整网关烟测与 RTU 驱动探针是在同一客户机里的独立测试步骤，不是同时混合负载；混合并发故障测试在 WSL 运行。
QEMU 故意断开 broker 时 publish_failures=3，随后恢复，不可描述为全程零发布失败。
RTU 探针故意制造一次响应超时和坏 CRC，其错误计数非零是预期注入结果。

## 调试与限制

- 首次 RTU 构建缺少 pipeline_metrics 直接包含；补齐后重建通过，保留 build-rtu.log 与 build-rtu-retry.log。
- 首次完整测试使用不支持的 MQM_TEST_LOGLEVEL=warn；改为 warning 后通过，保留 ctest-full.log 与 ctest-full-retry.log，未放宽断言。
- 本机未生成 format-check / tidy-naming 目标；lint.log 记录失败，没有标记为通过，未自动格式化。
- RTU 探针将 poll 剩余等待下限限制为零，避免负值造成无限等待，最终版本已重新构建并在 QEMU 验证。
- 原有 ExprTk 相关测试通过使用现有 /tmp/exprtk-src 依赖恢复执行，未修改原业务测试。
- 未连接真实 EMQX、未重跑长时间 soak、未进行硬件测试。本轮是迁移验收，不是新的性能极限结论。

## 复现

本机构建目录 /tmp/mqmgateway-edge-phase2-build；ARM 构建目录 /tmp/mqmgateway-edge-phase2-arm64。

```sh
MQM_TEST_LOGLEVEL=warning ctest --test-dir /tmp/mqmgateway-edge-phase2-build --output-on-failure -j2
ctest --test-dir /tmp/mqmgateway-edge-phase2-build -R rtu_driver_runtime_tests --output-on-failure
python3 tests/integration/mixed_protocol_test.py --binary /tmp/mqmgateway-edge-phase2-build/src/iot_gateway/mqmgateway_iot --output <新目录> --duration 30 --can-rate 100 --inject-faults
python3 scripts/run_buildroot_qemu.py --mode tests --binary-dir /tmp/mqmgateway-edge-phase2-arm64 --rtu-driver-probe --results <新目录>
```

首次完整 CTest 运行时包含原有三项；新增驱动测试随后单独运行。现在执行完整 CTest 会包含四项。
保留 docs/original_architecture.md、原 modmqttd/libmodmqttsrv 业务代码及历史证据。

# Phase 4.5 北向架构验证记录（2026-10-03）

基准 HEAD：`644187153744cf7ecb66ccdca9f9be44ba87e8f5`；测试基于含 Phase 1–5 及本轮尚未提交改动的工作区，**不是该 HEAD 单独的结果**。环境：WSL Ubuntu 24.04、GCC 13.3、libmosquitto 2.0.18；本地 Mosquitto + vcan/PTY/模拟 TCP/MC/Snap7/OPC UA。未跑本轮 ARM64/QEMU、真实硬件或云端 TLS。

## 主要结果

| 检查 | 证据 | 结果 |
| --- | --- | --- |
| 北向单元测试 | `northbound-unit-post-lock.log` | PASS：8 用例、102 断言；双适配器 fan-out、失败隔离、语义、命令状态、RTU ID 往返、有界队列 |
| 本地命令闭环 | `command-lifecycle-post-lock/summary.json`、`mqtt-observed.json` | PASS：有效命令只执行一次；重复 ID、过期和非法命令有明确回执；本地队列峰值 2，回执入队失败 0、待完成发布 0 |
| CAN + RTU | `mixed-can-rtu-post-lock/summary.json` | PASS：CAN 250/250、RTU 55/55，两个命令确认，零观察丢失、拒绝 0 |
| Modbus TCP + Generic TCP | `tcp-inflight/summary.json` | PASS：旧遥测/状态 topic 兼容，双向命令、非法命令拒绝 |
| MC 3E | `mc-inflight/summary.json` | PASS：D-word 小端写入、非法 D 地址拒绝 |
| 诊断兼容 | `diagnostics-inflight/summary.json` | PASS：队列卡滞/恢复和 MQTT ack 旧路径 |
| 默认依赖构建 | `/tmp/mqmgateway-phase4_5-default` | PASS：关闭 S7/OPC UA 可选依赖时仍能构建网关 |
| 完整 CTest | `ctest-final-test-wait.log` | PASS：11/11；此前 S7 间歇失败记录仍保留 |
| QoS0 遥测兼容 | `tcp-qos0-inflight/summary.json` | PASS：本地 broker 双向 TCP 路径，遥测使用 QoS0 |

## 失败记录，未删除

- `command-lifecycle-local/` 首次脚本在心跳刷新指标文件前读取旧快照而误报；`command-lifecycle-local-retry1/` 第二次脚本把布尔等待结果当字典；修正**测试脚本**后 `command-lifecycle-local-retry2/`、`command-lifecycle-final/` PASS。原 broker/gateway 日志与观测值保留。
- `diagnostics-regression-local/` 首次在 DrvFS 读取原子替换中的指标文件遇到 `OSError: No data available`；测试脚本仅补充 `OSError` 重试，`diagnostics-regression-local-retry1/` 和 `diagnostics-queued/` PASS。
- `mc-local/` 原网关收到错误 D 地址时，北向重构把“驱动提交前拒绝”映射成 `failed`，旧契约期待 `rejected`。已在命令管理器分类处作最小修正，`mc-local-retry1/` PASS。原始失败日志保留。
- `ctest-after-mc-fix.log` 和 `ctest-final-post-lock.log` 出现 S7 运行时单项间歇失败：`S7 bad address disconnect state`。S7 驱动先增加 `errors_`，随后将 `connected_` 置为 false；测试立即检查第二状态存在时序窗口。本轮只把测试断言改成有界等待，未修改 S7 驱动。修正后完整测试见 `ctest-final-test-wait.log`。

`ctest-native.log`、`ctest-native-final.log`、`ctest-post-flake.log`、`ctest-final-inflight.log`、`ctest-final-post-lock.log` 和 `ctest-final-test-wait.log` 保留不同时间点的完整 CTest 输出，不用一次通过掩盖另一次失败。具体源码/线程/兼容边界见 `docs/northbound_architecture.md`。

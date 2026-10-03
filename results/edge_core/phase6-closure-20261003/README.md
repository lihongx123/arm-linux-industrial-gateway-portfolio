# Phase 6 本地闭环证据（2026-10-03）

源码：基准 HEAD `644187153744cf7ecb66ccdca9f9be44ba87e8f5` 加当前未提交 Phase 1–6 工作区。环境：WSL Ubuntu 24.04、GCC 13.3、本地 Mosquitto、vcan0。未做云端和实体硬件验证。

| 证据 | 结果 |
| --- | --- |
| `focused-unit-final.log` | PASS：`[phase6],[northbound]` 共 12 用例、155 断言；含特殊字符告警 JSON 转义 |
| `watchdog-alarm-final/summary.json`、`mqtt-observed.json` | PASS：队列停滞、MQTT 确认、恢复；告警 `raised/acknowledged/recovered`，序号 1–7，网关退出 0 |
| `device-health-final/summary.json`、`mqtt-observed.json` | PASS：vcan 设备 `can-291`，stale/offline `raised` 后来帧 `recovered`，序号 1–2，网关退出 0 |
| `mixed-can-rtu-final/summary.json` | PASS：5 秒 CAN 250/250、RTU 55/55，零观察缺失/重复、双向命令确认、网关退出 0 |
| `command-lifecycle-final/summary.json` | PASS：本地 TCP 模拟设备，重复 ID 拒绝、过期超时、非法命令拒绝，回执发布失败 0 |
| `build-full.log`、`build-default.log` | PASS：启用与关闭 S7/OPC UA 两种构建 |
| `ctest-final.log` | PASS：完整 CTest 11/11，0 失败，181.43 秒 |

两个集成脚本的 `run-parameters.json`、`broker.log`、`gateway.log`、指标和 MQTT 观察记录均保留。早期 `results/edge_core/phase6-20261003/` 不覆盖、不改写。本轮北向新增独立告警主题，不能用早期只有诊断快照的结果冒充新增功能已通过。

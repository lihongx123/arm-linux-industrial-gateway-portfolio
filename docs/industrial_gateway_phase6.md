# Phase 6：健康、告警、诊断与看门狗

适用范围：当前未提交工作区的 `mqmgateway_iot`。基准 HEAD `644187153744cf7ecb66ccdca9f9be44ba87e8f5` 本身不含 Phase 1–6 全部改动。按用户要求，本阶段只进行本地模拟/软件验证，不把历史 ARM64 或云端结果归给当前二进制。旧 `modmqttd` 不走本框架。

## 运行设计

`GatewayCore` 注册设备健康记录；驱动消息只有通过设备/点位校验和 `PointMapper` 后才被 `DiagnosticsManager` 观测。`DeviceHealth` 记录好消息、错误与连续失败/恢复次数，诊断管理器将状态边沿交给 `AlarmLedger`。心跳线程定期检查 stale 与工作队列停滞、写指标，并将新告警交给北向有界队列；MQTT 发送不在诊断锁内发生。

默认 `failureThreshold=3`、`recoveryThreshold=2`、`staleAfter=0`（被动设备默认不按沉默判离线）。墙钟用于事件时间，单调时钟用于 stale/队列超时。健康状态为 `unknown`、`online`、`degraded`、`offline`；告警仅在边沿生成 `raised/updated/acknowledged/recovered`。历史最多保留 256 条；诊断快照最多展开 64 个设备，超过时带截断标记。

网关工作队列非空且出队计数在阈值内不变才报 `queue_stalled`；空队列是空闲。看门狗只报告，不自动重启或清队列。`--diagnostics-mqtt=1` 开启旧诊断主题 `gateway/{client}/diagnostics`、告警主题 `gateway/{client}/alarm`、确认请求 `/diagnostics/ack` 及回执 `/diagnostics/ack/result`。告警 JSON 字段为 `sequence,key,action,severity,epoch_ms`。入队成功才推进本地事件游标；这不是 broker/订阅者交付保证。历史被覆盖的序号缺口由 `alarm_history_missed` 计数。

## 源码入口

- `src/edge_core/device_health.*`：健康滞回和 stale 判定。
- `src/edge_core/alarm_event.*`：活跃告警、确认、有限历史、事件序号。
- `src/edge_core/diagnostics.*`、`time_service.hpp`：同步边界、快照、墙钟/单调时钟。
- `src/edge_core/watchdog.*`：工作队列进展观察。
- `src/edge_core/gateway_core.*`：映射后的诊断观测。
- `src/iot_gateway/gateway.cpp`：心跳评估、事件北向发布和指标。
- `src/northbound/mqtt_northbound_adapter.cpp`：告警 topic/JSON、确认请求。

参数默认值和调整意义见 [`GATEWAY_SOURCE_STUDY_GUIDE.md`](GATEWAY_SOURCE_STUDY_GUIDE.md)。

## 本地验证与剩余边界

证据见 [`phase6-closure-20261003`](../results/edge_core/phase6-closure-20261003/README.md)。相关单元测试 12 用例、155 断言 PASS；本地 Mosquitto/vcan 测试分别验证队列停滞的 `raised → acknowledged → recovered`，以及 CAN 设备 stale/offline 后再来帧恢复。完整 CTest **11/11 PASS**，0 失败；启用和关闭 S7/OPC UA 的构建均通过。本轮未运行云端、实体硬件或 ARM64 新二进制验证。

告警和命令结果只在内存中，不保证长期离线后的补投；看门狗不是逐线程心跳，也不能定位所有死锁。健康阈值需按设备更新周期配置。ADC/PWM 仍未实现，实体总线电气层仍未验证。

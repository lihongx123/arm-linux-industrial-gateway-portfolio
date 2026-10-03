# 工业网关技术文档：架构、参数与 ADC/PWM 仿真

更新：2026-10-04。入口是 `src/iot_gateway/main.cpp` 的 `mqmgateway_iot`；原上游 `modmqttd` 保留为独立程序。源码归属见 [PROVENANCE.md](PROVENANCE.md)。

## 数据与命令怎么流动

```text
CAN / RTU / TCP / UART / MC ─→ epoll Reactor ─┐
SPI / I2C / GPIO / ADC / PWM ─→ 采集调度器 ────┼─→ GatewayCore
S7 / OPC UA ────────────────→ 采集调度器 ────┘      │
                              DeviceRegistry + PointRegistry + PointMapper
                                                    │
                                  遥测有界队列 → worker → MQTT 适配器有界队列
MQTT 命令 → 命令有界队列 → GatewayCore → DriverManager → 目标驱动 → 状态回执
```

`GatewayCore` 只按设备/点位和驱动注册表路由，不把协议名称写进核心业务分支。Reactor 处理可读/可写 fd；板级采样通过共享的两个调度线程按单调时钟轮询。每个采集驱动同一时刻只执行一次。遥测、命令、MQTT 出站各有容量和拒绝指标；健康/告警/诊断由 Phase 6 提供，上报策略及控制面隔离由 Phase 6.5 提供。源码阅读顺序见 [GATEWAY_SOURCE_STUDY_GUIDE.md](GATEWAY_SOURCE_STUDY_GUIDE.md)。

## 常用参数

| 入口 | 默认值 | 作用 |
| --- | --- | --- |
| `--queue-capacity` / `--telemetry-queue-capacity` | 1024 | 遥测队列容量，满时拒绝新样本；两个参数后出现的值生效 |
| `--workers` | 2 | 遥测 worker 数 |
| `--command-queue-capacity` | 64 | 独立命令队列容量 |
| `--command-workers` | 1 | 命令 worker 数 |
| `--mqtt-outbound-capacity` | 默认跟随遥测容量 | MQTT 适配器总出站容量，内部为控制消息保留槽位 |
| `--mqtt-inflight` | 20 | QoS 发布在途上限 |
| `--telemetry-qos` | 1 | MQTT 遥测 QoS |
| `--heartbeat-ms` | 1000 | 网关在线状态周期 |
| `--health-failures` / `--health-recoveries` | 3 / 2 | 健康状态滞回阈值 |
| `--health-stale-ms` | 0 | 默认不以静默时间判设备离线；配置后启用 |
| `--point-policy` | 未配置 | 可按点位启用 COV、死区和最长上报间隔 |

队列增大只吸收突发，不提高持续处理能力；worker 增加也要同时观察 CPU、MQTT 在途窗口和实际出站速率。历史压力结果的配置与当前功能验证的配置分别列在 [EVIDENCE_INDEX.md](EVIDENCE_INDEX.md)。

## ADC：IIO 原始采样

生产入口 `--adc=id,point,iio_raw_path,poll_ms,scale,offset`，例如：

```text
--adc=adc01,voltage,/sys/bus/iio/devices/iio:device0/in_voltage0_raw,20,0.5,-10
```

`AdcBackend` 只接受 `/sys/bus/iio/devices/iio:deviceN/in_voltageM_raw` 形式的路径；一次读取一个非负、最多 32 位的十进制原始码，编码为 4 字节大端整数。`BoardDriver` 把它交给 `PointMapper`，工程值是 `raw × scale + offset`。因此示例原始码 2048 对应 1014。ADC 是只读点位，写命令被拒绝。这里的 scale/offset 由网关配置提供，不自动读取 IIO 的 `*_scale` 或 `*_offset` 属性；实际单位和标定系数应按具体传感器确定。

## PWM：已导出通道的安全输出

生产入口 `--pwm=id,point,pwm_channel_path,period_ns,poll_ms`，例如：

```text
--pwm=pwm01,duty_ns,/sys/class/pwm/pwmchip0/pwm0,1000000,20
```

通道应先由系统导出。`PwmBackend` 只接受 `/sys/class/pwm/pwmchipN/pwmM` 形式的路径；周期限制为 1～1,000,000,000 ns。启动时先禁用输出、清零占空比，再设置周期；命令负载是 **8 字节大端无符号占空比纳秒数**，必须不超过周期。写入顺序为禁用→写占空比→非零时启用。越界值在任何输出文件变更前被拒绝；写文件途中失败和正常停机都会尝试禁用并清零。定时读取 `duty_cycle` 作为点位回读值。示例 250000 / 1000000 = 25% 占空比。

`BoardDriver::submit()` 的同步 `status=ok` 表示 Linux 属性写入已完成；不能据此推断引脚实际波形、负载动作或外部设备反馈。权限、pwmchip 引脚复用、极性及电气参数仍应由目标板配置和仪器测量确认。

## 仿真方法和验证结果

`tests/integration/board_adc_pwm_simulation_tests.cpp` 创建独立临时 IIO/PWM 属性文件，通过**同一生产后端类**的显式 simulation 配置运行，不使用随机样本。它将两个驱动注册到 `GatewayCore`，由真实 `AcquisitionScheduler` 触发采集，检查映射值、输出写入/回读、非法占空比、非法 ADC 值和停机安全态。生产命令行没有 simulation 开关，不能把任意文件路径当作真实 IIO/PWM 设备启动。

本机单探针结果：`adc_scaled=1014`、`pwm_period_ns=1000000`、`pwm_duty_ns=250000`、非法占空比拒绝、负值 ADC 拒绝、PWM safe-off，全部 PASS。ARM64 Buildroot/QEMU 使用交叉编译后的同一测试逻辑在客户机内运行，`guest_exit=PASS`、QEMU 8.2.2、最终主机运行 34.647 秒。原始记录位于 [ADC/PWM 最终客体证据目录](../results/edge_core/adc-pwm-20261004/arm64-qemu-final/)，其中 `guest-results/results/board-adc-pwm-probe.json` 是客体结果，`injected-binaries.json` 给出二进制 SHA256。

最终本机 Release 全套 CTest 为 **12/12 PASS**，耗时 185.20 秒；[结构化摘要](../results/edge_core/adc-pwm-20261004/summary.json) 保留了本机与 ARM64 的结果。S7/OPC UA 在本机配置中开启，ARM64 交叉编译沿用已存在的可选依赖；未重新进行云端通信实验。

这次 QEMU 验证了 ARM64 Linux 构建、启动和软件采集/写入路径。QEMU 没有提供真实 ADC 电压或 PWM 引脚波形；这些属于后续实体板卡验证。

## 复现

```bash
cmake -S . -B /tmp/mqmgateway-gateway-build -DCMAKE_BUILD_TYPE=Release
cmake --build /tmp/mqmgateway-gateway-build -j4
ctest --test-dir /tmp/mqmgateway-gateway-build --output-on-failure
```

仅运行 ADC/PWM 软件探针：`/tmp/mqmgateway-gateway-build/src/iot_gateway/mqmgateway_board_adc_pwm_tests`。现有 ARM64 构建需要 `BUILDROOT_OUTPUT_DIR` 指向完整 Buildroot 输出目录；本轮第一次重新配置遗漏该变量而失败，补上已有 `/tmp/mqmgateway-br-output` 后交叉编译通过，未改变核心业务逻辑。QEMU 注入和提取入口为 `scripts/run_buildroot_qemu.py --mode tests --board-adc-pwm-probe --binary-dir <ARM64 build> --results <new empty directory>`。

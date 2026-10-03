# 可追溯测试证据

本索引只链接实际文件。测试时长、平台和版本是结论的一部分。

| 场景 | 入口 | 说明 |
| --- | --- | --- |
| 原始基线 | [baseline_build](baseline_build.md)、[原始记录](../results/baseline/) | 编译通过但原始测试存在缺依赖失败，后续补依赖回归 |
| x86原生负载 | [summary.csv](../results/summary.csv)、[load](../results/load/) | 不能当作ARM实机指标 |
| 故障恢复 | [fault](../results/fault/) | queue overflow、timeout、Broker重连 |
| ARM64构建 | [buildroot](../results/arm64/buildroot/)、[cross-build](../results/arm64/cross-build/) | 工具链、ELF和构建记录 |
| ARM64功能 | [qemu](../results/arm64/qemu/) | CAN双向、RTU流、非法命令与重连 |
| 历史阶梯 | [index.csv](../results/arm64/stress/guest-results/stress-results/index.csv) | 原始六档及两次1500边界复测 |
| 500 msg/s、8小时 | [结果](../results/arm64/soak/guest-results/soak-results/soak-result.json) | 28800秒、1440万条、零观察丢失 |
| 1500 msg/s、30分钟 | [结果](../results/arm64/stress/long-run-1500-1800s-20260922/guest-results/soak-results/soak-result.json) | 2700003条、零观察丢失 |
| 8天计划中止 | [说明](../results/arm64/stress/long-run-1500-8x24h-20260922/INTERRUPTED.md) | 用户主动中止，无最终PASS/FAIL结论 |
| 无界日志占满磁盘 | [incident](../results/arm64/soak-failed-nospace-20260921/incident.md) | 基础设施失败，保留原始证据 |
| 错误直方图预检 | [原始记录](../results/arm64/soak-histogram-invalid-120s-20260921/) | 零分位数无效，不计性能成果 |
| 本轮分段诊断 | [实验说明](pipeline_observability.md)、[pipeline](../results/arm64/pipeline/) | 同一二进制/QoS/开关对照与SHA256 |
| 扩展性与架构评审 | [报告](scalability_architecture_review.md)、[参数矩阵](../results/arm64/scalability/20260922-sweep/summary.md) | workers/CPU/队列/设备数/内存对照，未修改网关源码 |
| EMQX Cloud TLS扩展 | [配置与验证边界](cloud_mqtt.md)、[实时汇总](../results/cloud-live-summary.txt) | 本地/ARM64回归通过；真实EMQX Cloud 100 msg/s × 30 min与250 msg/s × 10 min均完成，详见链接证据 |
| Phase 4.5 北向架构 | [设计与验证边界](northbound_architecture.md)、[本地结果与失败记录](../results/edge_core/phase4_5-20261003/README.md) | 新源码的单元、CAN/RTU、TCP、命令生命周期与诊断本地回归；不代表本轮 ARM64/云端通过 |
| Phase 6 健康与告警 | [设计](industrial_gateway_phase6.md)、[本地闭环](../results/edge_core/phase6-closure-20261003/README.md) | 本地 vcan/Mosquitto 的队列停滞、设备 stale/offline、告警确认/恢复；不代表实体硬件或云端通过 |

原始日志可能包含本机目录、工具版本和上游测试中的示例用户名/口令。发布包不包含
私钥、账户凭据、虚拟机磁盘、工具链、编辑器状态或`.git`。打包清单记录每个包含文件
的SHA256；排除项不会从原始工作目录删除。

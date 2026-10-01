# 简历证据（仅限已实测）

可据实表述：

- 基于 C++17/Linux 扩展开源 Modbus–MQTT gateway，保留 Modbus RTU/TCP、YAML、converter 与双向 queue，新增 SocketCAN/epoll、统一消息、MQTT command routing、有界队列和固定 worker pool。
- 在 Ubuntu 24.04 WSL2 x86_64 上完成 Release build 与全量 CTest，最终 2/2 CTest 通过；功能、集成、负载和故障测试入口均 exit 0。
- 使用 vcan0 实测 100 个模拟设备、1500 CAN→MQTT msg/s、5 秒内 7500/7500 到达，P99 42.495 ms，CPU 6.20%，RSS 5.81 MB。
- 60 秒稳定性测试以 100 个设备、500 msg/s 发送 30000 条，收到 30000 条，P99 0.999 ms，CPU 3.22%，RSS 5.75 MB。
- Modbus TCP 1 ms poll 配置实测约 962.14 requests/s；PTY Modbus RTU slave 的 holding/input/coil/write、timeout、CRC error、invalid frame 和恢复共 8/8 通过。
- MQTT broker 断线恢复实测 1.539577 秒；容量 4 的 queue 故障测试真实触发 peak 4、overflow reject 641、command timeout 223，publish final failure 0。
- 容量边界：5000 CAN frames/s burst 时 MQTT 实收约 3027.35/s、loss 39.45%、P99 2758.40 ms；该结果作为风险和调优基线保留。
- Buildroot 2025.02.18 LTS 交叉编译产出 ARM64 ELF（aarch64-buildroot-linux-gnu GCC 13.4.0），QEMU `virt` guest 运行 Linux 6.12.27；ARM guest 的 RTU PTY 14 checks、CAN↔MQTT、非法命令拒绝、broker 重连和 500/500 性能消息全部 PASS，性能测试 loss=0。
- ARM64/QEMU 阶梯极限压测：1000 msg/s 为已验证的最高 30 秒稳定档（30000/30000、loss 0、P99 81.623 ms、网关进程 CPU 69.71%、RSS 3.31 MB）；1500 msg/s 在 30 秒进入退化档，并在两次 60 秒复测中达到失败阈值；2000 msg/s 实收持续吞吐约 1694.90 msg/s、loss 2.2033%、P99 4574.657 ms，为明确过载点。
- ARM64/QEMU 8 小时稳定性测试以 100 个设备、500 msg/s、2 workers、queue 1024 完成：持续 28800.000184 秒，发送/接收 14400000/14400000、loss 0、P50/P95/P99 2.4/7.5/52.7 ms、网关 PID CPU 50.138%、峰值 RSS 3.262 MB、queue peak 13、rejected/publish failure/timeout/CAN error 均为 0。CPU/RSS 由 Buildroot guest 内 `/proc/<pid>` 采集，不含 Windows 其他进程。
- 长稳测试过程中真实发现并保留两项测试基础设施问题：第一轮无界消息日志增长至 347248632 bytes 并耗尽 384 MiB guest 文件系统；随后一次延迟直方图初始化错误产生无效零分位数。两者均保留原始失败证据、仅修复测试工具，并通过短回归后重跑正式 8 小时测试。

## 2026-09-22更新

- 1500 msg/s在独立30分钟ARM64/QEMU测试中通过：1800.000416秒，2700003/2700003，
  P50/P95/P99为0.9/1.4/1.8 ms，网关CPU64.55%、峰值RSS3.297 MB。该记录与旧1500
  失败记录并存；不能直接声称它们之间的差异来自网关优化。
- 后续8天计划由用户主动中止，宿主运行15930.180秒；没有最终负载结果，不能写成8天通过。
- 新增可选分段观测：CAN接收/解析、遥测入队/出队、遥测与命令等待、MQTT接受与
  PUBACK区分、定长延迟统计和有界MID跟踪；相关测试与对照实验见pipeline目录。
- 2026-09-22重新构建后本地CTest仍为2/2通过（unit_tests 170.36秒）；新增
  pipeline_metrics_tests共5个测试、29个断言已纳入该二进制。
- 分段诊断显示：QoS1在3000 msg/s短时可稳定，4000 msg/s在窗口20/100及Broker窗口100
  下均未形成稳定档；QoS1在5000 msg/s出现约19.1667%丢失、P99约10.2946秒、pending峰值4096
  和Broker outgoing drop。QoS0在同条件约4991.06 msg/s、丢失0.1653%，但仅用于诊断，不能替代
  QoS1确认交付。该结果支持“下游发布/ACK/队列链路是主要瓶颈候选”，不证明单一根因。

禁止写入简历的内容：真实硬件 CAN/RS485 性能、尚未验收的公有云接入、没有同条件
对照证据的吞吐提升百分比。ARM64性能与稳定性数据必须明确标注Buildroot/QEMU。

## 配置扩展性与架构评审补充

完成4000 msg/s下workers=1/2/4/8、vCPU=2/4、队列256/1024/4096、
设备标识10/100/1000、内存512/1024/2048MiB的十二组60秒配置实验。
十二组均未达到稳定阈值，保留原始失败数据。各组CAN读取与MQTT库接受的遥测数均跟上输入，
但订阅端出现计数缺口，说明不能把端到端结果简单归因为CAN读取能力。
新增客户机网关、Broker、发生器/订阅器的独立进程及线程CPU采样，定位到共置资源竞争和
Broker串行处理限制的证据；提出发布背压、控制与遥测隔离、批处理等候选架构，尚未实施。
可以写“完成扩展性评估与瓶颈定位”，不能写“架构优化后稳定达到4000 msg/s”。
详见[架构评审与数据](scalability_architecture_review.md)。

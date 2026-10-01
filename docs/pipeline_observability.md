# 分段观测与瓶颈定位

本轮从2026-09-22开始。8天测试已按用户请求中止，详情见
[中止记录](../results/arm64/stress/long-run-1500-8x24h-20260922/INTERRUPTED.md)。

## 已知事实与待验证假设

- x86_64 WSL 原生测试与 ARM64 QEMU 测试是两套环境；Buildroot 构建系统，
  QEMU 执行 ARM64 镜像，vcan/PTY/模拟程序生成设备输入。
- 旧 ARM64 阶梯在1000 msg/s通过，1500出现退化，2000失败；但后续
  [1500 msg/s、30分钟测试](../results/arm64/stress/long-run-1500-1800s-20260922/guest-results/soak-results/soak-result.json)
  已通过，2700003/2700003，P99 1.8 ms。不能把1000称为所有配置的固有极限。
- 旧 probe 输出精确分位数，当前 probe 使用0.1 ms定长直方图；测量实现曾改变。
  跨轮差异不是优化效果的证据。新实验固定probe并记录SHA256。
- MQTT ACK/inflight 是假设。任务队列浅不能证明全链路无积压；CPU低于100%
  也不能单独证明阻塞、QEMU调度、Broker或ACK窗口中的任一原因。

## 观测开关和兼容性

```sh
mqmgateway_iot --mqtt-host=127.0.0.1 --mqtt-port=1883 \
  --client-id=pipeline --can-interface=vcan0 --workers=2 --queue-capacity=1024 \
  --pipeline-metrics=1 --telemetry-qos=1 --metrics-file=/tmp/pipeline.json
```

默认`pipeline-metrics=0`、`telemetry-qos=1`。命令订阅、状态和心跳维持QoS1。
`mqtt-inflight`默认20，允许1..65535，不接受无限窗口0。MQTT 3.x使用
`mosquitto_max_inflight_messages_set`设置窗口，启动时检查返回码。先通过实验选择值，
不会因某次高负载通过直接改变所有部署的默认配置。
QoS0仅用于遥测单因素诊断，不代表确认交付。新指标采用JSON追加字段；旧字段保留。
metrics先写临时文件，再原子替换，避免读到截断JSON。

| 阶段 | 观测量 | 边界 |
| --- | --- | --- |
| 输入发生器 | probe sent | SocketCAN write成功次数，非物理总线交付证明 |
| CAN读取/解析 | frames_read / parsed / rejected_frames / read_errors | 仅本Socket可见数据，不含内核丢帧计数 |
| 遥测队列 | telemetry_enqueued / telemetry_dequeued / telemetry_queue_wait | 与command_queue_wait分开 |
| 业务处理 | telemetry_worker_before_publish | worker开始到序列化结束，不含publish重试等待 |
| MQTT库 | publish_calls / publish_accepted / publish_api_errors | accepted表示库接收请求，不表示Broker收到 |
| MQTT完成 | puback_received / telemetry_puback_received | QoS1回调；QoS0仅记qos0_local_completed |
| 尚未完成 | pending_tracked / pending_peak / oldest_pending_us | 应用跟踪的未完成消息，**不等于线上的inflight窗口** |
| 订阅端 | probe received / P50/P95/P99 | 现有probe计数未做序列号去重，QoS1重复消息是局限 |

`published`旧字段等价于成功接受请求数。`processing_latency_ms`旧字段实际是
历史队列平均等待时间，不是业务执行时间。各阶段快照非全链路事务，运行中可能短暂不等；
最终排空后再核对守恒关系。心跳和状态属于控制流，不能与遥测输入直接比较总published。

StageLatency使用64桶定长对数直方图，字段`p99_upper_us`等是桶上界，
同时给出精确max和mean；不能把桶上界当作精确分位数。ACK跟踪最多4096个MID，
超过容量或MID冲突会设置tracking_complete=false并增加tracking_dropped。
它约束观测内存，不对MQTT库内部队列实施背压。

QoS0回调可能发生在publish调用内，使用递归互斥和提前完成匹配；QoS1跨线程
回调与MID注册在同一锁下同步。此锁会带来测量开销，必须对照关闭观测的运行。

网关内部阶段用steady_clock。端到端probe目前将system_clock时间戳放入8字节CAN负载，
不能与steady_clock直接相减。PUBACK和Broker转发可并行，订阅端可能先收到消息；
因此不能把“Tsubscriber−Tpuback”解释成独立云端耗时。超过60秒的延迟在旧probe
中归为malformed，这也是容量测试的测量局限，不能据此称协议解析错误。

## 对照方法

`scripts/run_pipeline_experiment.py`使用独立临时运行镜像，校验写入的网关二进制，
记录网关、probe、内核、基础镜像和源码SHA256。每档重启Broker和网关：

1. QoS1，关闭观测（测量开销参照）。
2. QoS1，开启观测。
3. QoS0，开启观测。

同一运行中的devices=100、workers=2、queue=1024和2vCPU/1GB保持一致。
实验完成不等于每档通过；看各档result.json的classification。先做30秒探索，
找到退化档后做60秒以上独立重复；未验证的提升不写入简历。

```sh
python3 scripts/run_pipeline_experiment.py \
  --gateway /tmp/mqmgateway-pipeline-arm64 \
  --results results/arm64/pipeline/NEW-UNIQUE-RUN \
  --seconds 30 --rates 1000,1500,2000,3000,5000
```

窗口对照保持QoS1和观测开启：

```sh
python3 scripts/run_pipeline_experiment.py \
  --gateway /tmp/mqmgateway-pipeline-window-arm64 \
  --results results/arm64/pipeline/NEW-WINDOW-RUN \
  --seconds 30 --rates 3000,5000 --variants 1:1:20,1:1:100,1:1:200
```

旧`run_buildroot_qemu.py`会清空对应输出目录，只能在新副本中复现历史入口；
新诊断入口拒绝已存在目录。不要并行启动容量实验。

## 后续优化的准入依据

- ACK延迟和pending随负载上升：检查发布窗口、Broker限额、网络loop和背压。
- Queue wait上升而ACK稳定：测1/2/4 workers，评估锁争用及处理能力。
- 遥测突发导致command等待：增加独立指令调度，并验证deadline，不只看吞吐。
- accepted和ACK正常而subscriber不足：检查Broker下游队列、订阅端及发生器能力。

云接入属于后续阶段，需要用户选定平台、MQTT端点、证书和测试设备凭据。
凭据通过本地环境或忽略的配置注入；未做云端连接、TLS验收或设备注册时，不写“已上云”。

## 当前实验结论（2026-09-22）

本节只汇总已落盘的 ARM64 Buildroot/QEMU 证据，不把探索性结果写成默认配置承诺。
首次30秒矩阵与原生 CTest 有时间重叠，因此仅作诊断参考；后续窗口复测为独立运行。

| 诊断条件 | 观测结果 | 解释 |
| --- | --- | --- |
| QoS1、观测关闭、1500 msg/s、30秒 | 实际约1500 msg/s，P99 2.3 ms，CPU 66.78% | 该档在诊断矩阵中稳定 |
| QoS1、观测开启、1500 msg/s、30秒 | 实际约1500 msg/s，P99 1.9 ms，CPU 65.08% | 观测开关未显示明显吞吐损失 |
| QoS0、观测开启、5000 msg/s、30秒 | 实际4991.06 msg/s，丢失248（0.1653%），P99 79.8 ms | 退化但未进程退出；仅为QoS0诊断，不代表确认交付 |
| QoS1、观测开启、5000 msg/s、30秒 | 实际3171.26 msg/s，丢失28750（19.1667%），P99 10.2946 s，RSS峰值18.75 MB | 明确失败；pending峰值4096、tracking_dropped 110392，Broker日志出现 outgoing messages dropped |
| QoS1、观测开启、3000 msg/s、60秒，窗口20 | 实际2999.88 msg/s，P99 38.5 ms，队列峰值25 | 独立复测稳定 |
| QoS1、观测开启、4000 msg/s、60秒，窗口20/100 | 两档均失败；实际约3419.81/3104.89 msg/s，丢失8571/51751 | 增大客户端窗口没有把4000 msg/s变成稳定档 |
| QoS1、观测开启、Broker窗口100、4000 msg/s、60秒 | 实际3313.68 msg/s，丢失10963，P99 10.2065 s | 调大Broker窗口仍失败 |

窗口20、100、200在3000 msg/s的不同轮次出现过稳定和退化，说明该区间受运行时调度/队列状态影响；
不能据此声称“窗口越大越快”。当前可复现结论是：1500 msg/s已有30分钟零丢失证据，3000 msg/s是
短时诊断中的临界区，4000 msg/s以上在当前2 worker/1024队列/2vCPU/1GB配置下未形成稳定档，
QoS1失败时的主要信号是下游发送堆积、ACK等待和Broker丢弃，而不是CAN解析错误。下一步应优先做
独立的发布背压/ACK窗口实验，再评估worker扩展；不应直接把更大的窗口或QoS0写入默认生产配置。

后续已完成配置扩展性实验，见[扩展性与架构评审](scalability_architecture_review.md)。
其中4vCPU组Broker单线程接近满核，说明上面的边界属于共置测试链路，不能当作网关独立容量。
本轮没有执行架构修改；架构候选及其业务取舍已列入评审，等待用户指导。

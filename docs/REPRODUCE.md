# 构建和复现

推荐Ubuntu 24.04（可在WSL2中运行），源码解压/克隆到Linux文件系统。原始测量
环境为Windows/WSL2，详情见baseline与ARM64报告。命令使用仓库根目录为工作目录。

## 原生构建

```sh
sudo apt-get update
sudo apt-get install -y build-essential cmake git pkg-config catch2 libfmt-dev \
  libmodbus-dev libmosquitto-dev libspdlog-dev libyaml-cpp-dev rapidjson-dev \
  mosquitto mosquitto-clients can-utils iproute2 socat python3-pexpect
git clone https://github.com/ArashPartow/exprtk.git /tmp/mqm-exprtk
git -C /tmp/mqm-exprtk checkout 1e4a80b5ec9b4832ed59c6faa65f625a01b18ef0
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Release \
  -DEXPRTK_INCLUDE_DIR=/tmp/mqm-exprtk -DBUILD_STRESS_TOOLS=ON
cmake --build build-native -j2
ctest --test-dir build-native --output-on-failure
```

ExprTk是可选插件依赖，但原始测试含依赖它的用例。缺失时配置成功不等于全量测试通过。
原始失败证据保留在`results/baseline/`；不要因预期不同删除失败用例。

## CAN诊断开关

```sh
sudo modprobe vcan
sudo ip link add vcan0 type vcan
sudo ip link set vcan0 up
mosquitto -p 18883
```

另一个终端：

```sh
build-native/src/iot_gateway/mqmgateway_iot --mqtt-port=18883 \
  --can-interface=vcan0 --pipeline-metrics=1 --telemetry-qos=1 \
  --metrics-file=/tmp/mqm-metrics.json
```

vcan仅为本机模拟接口。不要将匿名测试Broker监听在公网。QoS0仅作为遥测诊断变量。

## ARM64

使用Buildroot 2025.02.18，官方源包SHA256：
`00c772f86a60db0dc726ccc1bed8b6c83b8aa516f799032557e0e59d33eed95c`。
配置在`buildroot/configs/mqmgateway_aarch64_defconfig`，内核CAN配置在
`buildroot/linux-can.fragment`。安装QEMU、e2fsprogs、pexpect和Buildroot主机依赖后：

```sh
# 将核验过的Buildroot源码解压到 /tmp/buildroot-2025.02.18
bash scripts/build_buildroot_arm64.sh
```

历史构建/测试脚本使用固定`results/arm64/`目录，重跑可能覆盖该目录。
请在**新的源码副本**运行这些历史入口，保留已提交的证据。针对分段诊断，入口拒绝
重复结果目录，并分配独立运行镜像：

```sh
python3 scripts/run_pipeline_experiment.py \
  --gateway /tmp/mqmgateway-arm64-build/src/iot_gateway/mqmgateway_iot \
  --results results/arm64/pipeline/my-unique-run --seconds 30 --rates 1000,1500,2000
```

新gateway必须包含pipeline开关。请勿将旧阶梯、更新后的probe、不同Broker配置结果
拼成优化前后对照。运行参数、指纹和结果均应一起保留。

## 如何检查证据

`result.json`是负载结果，`gateway-metrics.json`是网关计数，`host-run`记录宿主执行，
`qemu-console.log`记录客户机输出。新矩阵的`MATRIX_COMPLETED`只表示各档执行结束，
并不表示每档STABLE。`pending_tracked`不是Mosquitto在线inflight数量。

现有脚本未完成物理CAN/RS485验收。WAN/TLS与公有云结论仅限
[`cloud_mqtt.md`](cloud_mqtt.md)记录的两次真实EMQX Cloud运行，不外推为硬件或云容量上限。

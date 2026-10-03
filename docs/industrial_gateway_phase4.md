# Phase 4：板级采集与串口设备支持（2026-10-02）

基线 HEAD：`644187153744cf7ecb66ccdca9f9be44ba87e8f5`。本轮先完成 [Phase 3 闭环审计](../results/edge_core/phase3-closure-20261002.md)，再实施 Phase 4；Phase 1–3 的未提交工作及旧证据保留。本轮没有提交或推送。本文只描述 `mqmgateway_iot` 新架构，不把旧 `modmqttd` 视为已迁移。

## 架构与运行路径

`GatewayCore` 仍是唯一设备/点位派发核心。`DriverManager` 注册 `IDeviceDriver`；可读 fd 的 CAN、RTU、Modbus TCP、Generic TCP、Raw UART 经 `SouthboundReactor` 的 epoll 回调；SPI、I2C、GPIO 经一个固定 **2 条共享工作线程** 的 `AcquisitionScheduler` 单次定时采集。最多注册 256 个采集驱动，每个驱动同一时刻最多有一个采集任务。调度使用单调时钟、轮转选取和 `runs/late/errors` 指标；一个慢设备占用一条线程时，另一条仍能处理其余设备。驱动调用阻塞的 Linux ioctl，因此若**两条**线程同时陷入不可中断内核调用，停机仍会等待内核返回；未宣称对此情形有硬实时保证。

每条遥测都走 `driver → DeviceRegistry → PointRegistry/PointMapper → UnifiedMessageV2 → 既有有界队列/worker → MQTT`。板级设备的设备/点位在启动前显式注册；Raw UART 亦然。`GatewayCore` 没有协议判断分支；采集调度器没有进入 epoll。写命令经现有 `CommandRouter → GatewayCore.prepare/submit → DriverManager` 检查设备、点位写权限和驱动能力，驱动发出 `status=ok/error/unavailable`。同步板级写入的 `submit=true` 表示命令已被驱动接受，结果以 status 表达；不等同物理设备最终动作确认。

| 驱动 | 生产输入和系统调用 | 点位原始值/映射 | 状态 |
| --- | --- | --- | --- |
| SPI | `/dev/spidevX.Y`，`open`，模式/位宽/速度 ioctl，`SPI_IOC_MESSAGE(1)` 全双工传输 | 命令前缀后的接收字节；当前配置按无符号大端整数读并做 scale/offset | IMPLEMENTED_NOT_HARDWARE_VALIDATED |
| I2C | `/dev/i2c-X`，`open`，`I2C_RDWR` 组合寄存器写＋读及独立写 | 7 位地址设备的接收字节；同一整数/标定点模型 | IMPLEMENTED_NOT_HARDWARE_VALIDATED |
| GPIO | `/dev/gpiochip*`，GPIO v2 字符设备 line request、GET/SET values ioctl | 单字节布尔值；active-low 由内核 line flag 处理 | IMPLEMENTED_NOT_HARDWARE_VALIDATED |
| Raw UART | 共享 `SerialPort` 的 `termios`/非阻塞 fd，Reactor 读写 | 定界符或固定长度帧的字节 hex；超长帧整帧丢弃 | VERIFIED_SOFTWARE（PTY）；物理 UART 未验证 |
| RS485 模式 | `SerialPort` 配置 `serial_rs485` 并调用 `TIOCSRS485` | 供 RTU 或 Raw UART 使用，不是独立应用协议 | VERIFIED_SOFTWARE（配置/错误路径）；电气层未验证 |

RTU 继续使用已有 CRC/事务解析器，只复用串口 open/termios/RS485 配置；Raw UART 不解析或伪装 Modbus。`SPI/ I2C/GPIO` 的确定性 `FakeBackend` 仅定义在 `tests/integration/phase4_software_tests.cpp`，生产目录使用 Linux 系统调用，不产生随机传感器值。

## 配置示例

以下只展示参数格式；实际 `/dev/*` 路径、芯片地址、命令字节及线号要按真实设备手册确认。未接硬件时不能把这些示例当作已验证硬件配置。

```text
--spi=adc01,channel0,/dev/spidev0.0,0,1000000,8,100,2,00,0.5,-10,0
--i2c=temp01,temperature,/dev/i2c-1,72,00,2,200,0.5,-10,0
--gpio=door01,open,/dev/gpiochip0,12,1,0,100
--uart=serial_sensor01,value,/dev/ttyS1,115200,10,250,1
--uart-rs485=1,1,0,2,3
--rtu-device=/dev/ttyS2 --rtu-rs485=1,1,0,2,3
```

SPI 字段是 `id,point,path,mode,speed_hz,bits_per_word,poll_ms,read_len,command_hex,scale,offset,writable`；当前仅支持 8 位字宽，模式 0–3。I2C 字段是 `id,point,path,7bit_address,register_hex,read_len,poll_ms,scale,offset,writable`，地址按十进制输入且只允许 `0x08..0x77`。GPIO 字段是 `id,point,path,line,active_low,output,poll_ms`；输出点可写。UART 字段是 `id,point,path,baud,delimiter_byte|fixed:N,reconnect_ms,writable[,bits,parity,stops]`，定界符十进制 `10` 表示换行。RS485 字段是 `enabled,rts_on_send,rts_after_send,before_ms,after_ms`；`--uart-rs485` 修饰紧邻其前的最后一个 `--uart`。所有字节命令以十六进制输入；标定参数拒绝非有限值及数字后缀。

## 软件验证与证据

证据目录：[Phase 4 原始日志](../results/edge_core/phase4-20261002/)。完整本机 Release/Ninja 构建与 CTest：`build-final.log`、`ctest-final.log`，**6/6 PASS，unit_tests 170.36 s**；独立 `[edge]` **9 用例/104 断言 PASS**（`edge-test.log`）。本地 MQTT Modbus TCP/Generic TCP E2E：`mqtt-e2e/summary.json` PASS。CAN/RTU 同进程 10 秒回归：`mixed-can-rtu/summary.json`，CAN 1000/1000、RTU 83/83、无缺失/重复、两个命令确认；网关单核 CPU 平均 1.99%、峰值 RSS 6272 KiB、队列峰值 2。这是该短回归的测量，不是 Phase 4 吞吐上限。

新增 `MIXED_SOFTWARE_INTEGRATION` 在**同一个 GatewayCore** 同时运行 8 个不同设备/点：真实 vcan0、真实 PTY RTU、本地 TCP Modbus 与 Generic、测试专用 SPI/I2C/GPIO 后端、真实 PTY Raw UART。`mixed-native.json` 是本机结果：`driver_messages=22`、`dropped=0`、`mapping_failures=0`、`commands_submitted=3`、`commands_rejected=0`；被注入故障的 SPI 最终 `healthy=false` 是预期状态。还检查了 UART 半帧/同读多帧、4097 字节超长帧完整丢弃、写入、PTY 替换后重开；短读/IO 故障及慢 SPI 期间 I2C 继续采集。该组成测试直接调用 GatewayCore 的发布回调，不经过 MQTT worker 队列，故只将上述队列指标归于另一个真实网关回归。

Buildroot ARM64/QEMU 使用重新交叉编译的二进制注入临时镜像；最终 `qemu-final-v2/host-run.txt` 显示 `guest_exit=PASS`、QEMU 8.2.2、主机侧 QEMU 运行时长 35.808 s。`qemu-final-v2/guest-results/results/` 中旧 CAN/MQTT/RTU 流冒烟、RTU 驱动探针、双 TCP 驱动探针及 8 设备 Phase 4 组成探针均 PASS；后者 `driver_messages=24`、`mapping_failures=0`、`reopens=2`、`oversized=1`、峰值 RSS 3428 KiB、测试进程累计 CPU 76 ms。其 SPI/I2C/GPIO 仍是测试专用后端，不是 QEMU 模拟的板级真实硬件。镜像注入日志中的 `debugfs rm: File not found` 是目标文件原先不存在时的删除提示；随后写入并运行成功。

## 调试与边界

- 新测试最初因缺少 board include 路径、`poll.h`、原子计数读取写法编译失败；补齐构建/测试依赖后编译通过。
- Raw UART 在 PTY 的 `VMIN=0,VTIME=0` 非阻塞读耗尽后返回 0，被误当断线；修为只在错误/HUP 时断开，新增真实重连及超长帧整帧丢弃测试。
- 调度器原为一条工作线程，慢 SPI 可阻塞其他设备；改成固定两条共享线程并用 600 ms 慢设备测试验证另一设备继续采集。
- 首次完整 CTest 的旧 FakeDriver 用空字节发布整数点位，现行 PointMapper 正确拒绝；修正测试输入为 `{0,42}` 而非放宽生产校验，`ctest-all.log` 保留失败，`ctest-final.log` 全部通过。
- 首次 ARM64 增量编译缺少 `BUILDROOT_OUTPUT_DIR` 环境变量；`build-arm64.log` 保留报错，设为既有完整 Buildroot output 后 `build-arm64-v2.log` 通过。
- 一次 QEMU 复跑的旧客体脚本只消费 CAN status topic 的第一条消息，恰好读到上一条有效命令迟到的 `ok`，使无效命令验收误报失败；`qemu-final/` 保留原始失败证据。脚本改为等明确 `rejected` 状态，并通过临时镜像注入更新后的测试脚本；`qemu-final-v2/` 再次 PASS。未为此修改 CAN/核心业务逻辑。
- 仓库说明要求运行 `format-check`/`tidy-naming`，但当前生成的本机构建没有这些目标，`lint-targets.log` 记录 `unknown target 'format-check'`；没有用自动格式化替代检查。`git diff --check` 通过，仅有 Git 对 CRLF 转换的提示。
- SPI/I2C 的实际总线时序、目标芯片寄存器语义、GPIO 电平/抖动、RS485 收发方向/终端电阻及真实板子上的延迟均未测试。GPIO 当前是轮询，不提供边沿事件或去抖。SPI 命令前缀+全双工读是本项目当前设备访问策略，不是通用传感器协议。Raw UART 的字节帧仍需由具体设备协议进一步解释。

## ADC / PWM 后续接口

两者状态均为 **NOT_IMPLEMENTED_DEFERRED**，未新增空驱动。ADC 拟用 Linux IIO 的 `/sys/bus/iio/devices/iio:deviceX` 标准属性或字符缓冲接口（需明确通道、比例、扫描布局），作为 `IDeviceDriver + IAcquisitionDriver` 接入点模型；验收需真实后端、配置/越界/短读测试、定时采集及硬件标定核对。PWM 拟用 Linux PWM subsystem 的 `pwmchip` 控制接口，作为受权限保护的可写点，配置周期、占空比、极性与停机安全态；验收需真实内核接口、范围校验、命令状态、停机行为及实际波形验证。Phase 5 的 S7/MC/OPC UA 在本轮未实施，进入下一阶段前仍需确定依赖版本/许可、点位配置和 ARM64 交叉编译方案；Phase 6 的完整健康/告警/诊断/看门狗未实施。

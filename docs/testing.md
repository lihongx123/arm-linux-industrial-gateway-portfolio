# 测试系统

## 入口

- `bash run_functional_tests.sh`：build、MQTT/Modbus TCP 闭环、RTU simulator、CAN 双向。
- `bash run_integration_tests.sh`：MQTT roundtrip、CAN 双向、断线恢复/积压。
- `bash run_load_test.sh`：场景 A–E。
- `bash run_load_test.sh --stability-only`：60 秒场景 J。
- `bash run_fault_test.sh`：Modbus timeout/CRC/invalid、invalid CAN、broker reconnect、queue overflow/timeout。

WSL `/mnt/c` 未启用 metadata，脚本需显式用 `bash` 启动；这是文件系统 executable bit 限制，不影响内容执行。CAN 测试前需存在 UP 状态的 `vcan0`。

## 最终结果

- Release build：PASS。
- CTest：1/1 PASS，0 fail，170.37 秒。
- MQTT roundtrip：20/20，0 fail；最终统一运行 P50 4.435 ms、P95 8.432 ms、P99 41.234 ms。
- Modbus RTU：8/8 functional/fault/recovery checks PASS。
- CAN：双向、invalid command、invalid error/RTR frame 共 4 项 PASS。
- functional/integration/fault 三个入口：全部 exit 0。

原始日志位于 `results/baseline`、`results/functional`、`results/load`、`results/fault`。测试是 x86_64 WSL2 结果，不代表 ARM 性能。

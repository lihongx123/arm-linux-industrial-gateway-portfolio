# 数据流

## Modbus 上行与下行

上行保持原项目链路：Modbus RTU/TCP → `ModbusThread` → `mFromModbusQueue` → `ModMqtt`/converter → MQTT state/availability。下行保持 MQTT command → `MqttObjectCommand` → `mToModbusQueue` → Modbus write。

## CAN 上行

1. `CanSocket` 通过 epoll 等待 `vcan0`/真实 CAN interface。
2. 标准 CAN data frame 转换为 `UnifiedMessage`；CAN error/RTR frame 被隔离。
3. 消息进入 `BoundedQueue`，满队列采用 reject-new 策略。
4. 固定 worker 将消息序列化为 JSON。
5. 发布到 `device/can-{decimal_id}/telemetry`。

## CAN 下行

1. Mosquitto 订阅 `device/+/cmd/+`。
2. `CommandRouter` 校验 topic 和 `can_tx` JSON payload。
3. 生成 southbound `UnifiedMessage` 并入队。
4. worker 检查 command deadline，未过期则发送真实 CAN frame。
5. 结果发布到 `device/{id}/status`。

## 实测闭环

`cansend vcan0 123#01020304` 实际得到 `device/can-291/telemetry`，payload `01020304`；向 `device/can0/cmd/can_tx` 发布 CAN ID 1110/`deadbeef` 后，`candump` 实际捕获 `456#DEADBEEF`。

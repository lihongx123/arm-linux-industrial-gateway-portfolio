# 协议与 topic

## MQTT

| Topic | 方向 | Payload |
|---|---|---|
| `device/can-{id}/telemetry` | gateway → broker | `UnifiedMessage` JSON |
| `device/{id}/cmd/can_tx` | broker → gateway | `{"can_id":291,"data":"0102","timeout_ms":1000}` |
| `device/{id}/status` | gateway → broker | command success/rejected/timeout/error JSON |
| `gateway/{client}/heartbeat` | gateway → broker | online、queue depth、queue peak |
| `gateway/{client}/status` | gateway → broker | retained online/offline |

`can_id` 支持 29 bit 范围；classic CAN payload 为 0–8 bytes，因此 `data` 最多 16 个 hex 字符。奇数长度、非 hex、越界 ID、未知 command 均拒绝。

## UnifiedMessage JSON

字段包括 `timestamp_ms`、`device_id`、`protocol`、`direction`、`data_type`、`address`、`payload` 和 `quality`。payload 使用无分隔的小写 hex，避免二进制经过 MQTT/JSON 时损坏。

## Modbus RTU simulator

模拟器处理 function code 01、03、04、05、06、15、16，逐帧验证 CRC16，并能一次性注入 timeout、错误 CRC 和非法短帧。`results/functional/modbus/rtu-frames.csv` 保存真实 request/response bytes。

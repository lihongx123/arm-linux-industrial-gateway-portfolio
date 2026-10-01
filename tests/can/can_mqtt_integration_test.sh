#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/../.." && pwd)"
result_dir="${RESULT_DIR:-$repo_dir/results/functional/can}"
gateway_bin="${IOT_GATEWAY_BIN:-/tmp/mqmgateway-release-build/src/iot_gateway/mqmgateway_iot}"
broker_port=18884
broker_pid_file="$result_dir/broker.pid"

mkdir -p "$result_dir"
ip link show vcan0 >/dev/null

cleanup() {
    for pid_name in subscriber_pid candump_pid gateway_pid; do
        pid="${!pid_name:-}"
        [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true
    done
    if [[ -s "$broker_pid_file" ]]; then
        kill "$(<"$broker_pid_file")" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

bash "$repo_dir/tests/mqtt/start_broker.sh" "$broker_port" "$result_dir/broker.log" "$broker_pid_file"

"$gateway_bin" \
    --mqtt-port="$broker_port" \
    --client-id=phase5 \
    --can-interface=vcan0 \
    --queue-capacity=64 \
    --workers=2 \
    --heartbeat-ms=200 \
    --metrics-file="$result_dir/metrics.json" >"$result_dir/gateway.log" 2>&1 &
gateway_pid=$!

mosquitto_sub -h 127.0.0.1 -p "$broker_port" -W 10 -C 1 \
    -t gateway/phase5/status >"$result_dir/online-status.json"

stdbuf -oL mosquitto_sub -h 127.0.0.1 -p "$broker_port" \
    -t 'device/+/telemetry' -t 'device/+/status' -F '%t %p' >"$result_dir/mqtt-messages.log" &
subscriber_pid=$!
sleep 0.2

timeout 10 candump -L vcan0 >"$result_dir/can-frames.log" 2>&1 &
candump_pid=$!
sleep 0.1

# Kernel error and RTR frames are invalid telemetry and must be isolated.
python3 "$script_dir/send_invalid_can.py" --interface vcan0
sleep 0.2
if grep -q 'device/can-1/telemetry' "$result_dir/mqtt-messages.log"; then
    invalid_can_rejected=fail
else
    invalid_can_rejected=pass
fi

# South device -> SocketCAN -> gateway -> unified message -> MQTT telemetry.
cansend vcan0 123#01020304
for _ in {1..100}; do
    if grep -q 'device/can-291/telemetry.*"address":291.*"payload":"01020304"' "$result_dir/mqtt-messages.log"; then
        can_to_mqtt=pass
        break
    fi
    sleep 0.02
done
can_to_mqtt="${can_to_mqtt:-fail}"

# MQTT command -> validated route -> queue/worker -> SocketCAN.
mosquitto_pub -h 127.0.0.1 -p "$broker_port" -q 1 \
    -t device/can0/cmd/can_tx -m '{"can_id":1110,"data":"deadbeef","timeout_ms":1000}'
for _ in {1..100}; do
    if grep -qi '456#DEADBEEF' "$result_dir/can-frames.log"; then
        mqtt_to_can=pass
        break
    fi
    sleep 0.02
done
mqtt_to_can="${mqtt_to_can:-fail}"

# Invalid payload must be rejected with an MQTT status rather than transmitted.
mosquitto_pub -h 127.0.0.1 -p "$broker_port" -q 1 \
    -t device/can0/cmd/can_tx -m '{"can_id":1,"data":"not-hex"}'
for _ in {1..100}; do
    if grep -q 'device/can0/status.*"status":"rejected"' "$result_dir/mqtt-messages.log"; then
        invalid_rejected=pass
        break
    fi
    sleep 0.02
done
invalid_rejected="${invalid_rejected:-fail}"

sleep 0.3
kill -TERM "$gateway_pid"
wait "$gateway_pid"
gateway_pid=

printf 'can_to_mqtt=%s\nmqtt_to_can=%s\ninvalid_command_rejected=%s\ninvalid_can_rejected=%s\n' \
    "$can_to_mqtt" "$mqtt_to_can" "$invalid_rejected" "$invalid_can_rejected" >"$result_dir/summary.txt"
cat "$result_dir/summary.txt"

[[ "$can_to_mqtt" == pass && "$mqtt_to_can" == pass && "$invalid_rejected" == pass && "$invalid_can_rejected" == pass ]]

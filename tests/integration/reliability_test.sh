#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/../.." && pwd)"
result_dir="${RESULT_DIR:-$repo_dir/results/fault/reliability}"
gateway_bin="${IOT_GATEWAY_BIN:-/tmp/mqmgateway-release-build/src/iot_gateway/mqmgateway_iot}"
broker_port=18885
broker_pid_file="$result_dir/broker.pid"

mkdir -p "$result_dir"
ip link show vcan0 >/dev/null

cleanup() {
    for pid_name in subscriber_pid gateway_pid; do
        pid="${!pid_name:-}"
        [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true
    done
    if [[ -s "$broker_pid_file" ]]; then
        kill "$(<"$broker_pid_file")" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

bash "$repo_dir/tests/mqtt/start_broker.sh" "$broker_port" "$result_dir/broker-before.log" "$broker_pid_file"
"$gateway_bin" \
    --mqtt-port="$broker_port" --client-id=reliability --can-interface=vcan0 \
    --queue-capacity=4 --workers=1 --heartbeat-ms=100 --processing-delay-ms=5 \
    --metrics-file="$result_dir/metrics.json" >"$result_dir/gateway.log" 2>&1 &
gateway_pid=$!

mosquitto_sub -h 127.0.0.1 -p "$broker_port" -W 10 -C 1 \
    -t gateway/reliability/status >"$result_dir/online-before.json"
stdbuf -oL mosquitto_sub -h 127.0.0.1 -p "$broker_port" \
    -t 'gateway/reliability/#' -t 'device/#' -F '%t %p' >"$result_dir/messages.log" 2>&1 &
subscriber_pid=$!
sleep 0.3
grep -q 'gateway/reliability/heartbeat' "$result_dir/messages.log"

# Drop the broker, inject a CAN frame while MQTT is unavailable, then restart it.
kill "$(<"$broker_pid_file")"
for _ in {1..50}; do
    if ! kill -0 "$(<"$broker_pid_file")" 2>/dev/null; then break; fi
    sleep 0.02
done
disconnect_ns="$(date +%s%N)"
cansend vcan0 321#A1B2C3D4
sleep 0.5
bash "$repo_dir/tests/mqtt/start_broker.sh" "$broker_port" "$result_dir/broker-after.log" "$broker_pid_file"

recovered=false
for _ in {1..300}; do
    if grep -q 'device/can-801/telemetry.*"payload":"a1b2c3d4"' "$result_dir/messages.log"; then
        recovered=true
        break
    fi
    sleep 0.02
done
reconnect_ns="$(date +%s%N)"

# Saturate a deliberately tiny queue. Short deadlines make queued commands expire.
for _ in {1..3000}; do
    printf '%s\n' '{"can_id":1110,"data":"01","timeout_ms":1}'
done | mosquitto_pub -h 127.0.0.1 -p "$broker_port" -q 0 \
    -t device/can0/cmd/can_tx -l
sleep 1

kill -TERM "$gateway_pid"
wait "$gateway_pid"
gateway_pid=

python3 - "$result_dir/metrics.json" "$result_dir/summary.txt" "$disconnect_ns" "$reconnect_ns" "$recovered" <<'PY'
import json, pathlib, sys

metrics_path, summary_path, disconnected, reconnected, recovered = sys.argv[1:]
metrics = json.loads(pathlib.Path(metrics_path).read_text(encoding="utf-8"))
reconnect_s = (int(reconnected) - int(disconnected)) / 1_000_000_000
summary = {
    "broker_recovered": recovered == "true",
    "reconnect_time_s": round(reconnect_s, 6),
    "queue_rejected": metrics["rejected"],
    "command_timeouts": metrics["command_timeouts"],
    "publish_failures": metrics["publish_failures"],
    "queue_peak": metrics["peak_depth"],
    "processing_latency_ms": metrics["processing_latency_ms"],
}
pathlib.Path(summary_path).write_text("\n".join(f"{key}={value}" for key, value in summary.items()) + "\n", encoding="utf-8")
print(json.dumps(summary, indent=2))
if not summary["broker_recovered"] or summary["queue_rejected"] == 0 or summary["command_timeouts"] == 0:
    raise SystemExit(1)
PY

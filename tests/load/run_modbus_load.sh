#!/usr/bin/env bash
set -euo pipefail
script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/../.." && pwd)"
result_dir="$repo_dir/results/load/Modbus_TCP_poll"
broker_pid_file="$result_dir/broker.pid"
mkdir -p "$result_dir"

cleanup() {
    for pid_name in gateway_pid modbus_pid; do
        pid="${!pid_name:-}"
        [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true
    done
    if [[ -s "$broker_pid_file" ]]; then kill "$(<"$broker_pid_file")" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

bash "$repo_dir/tests/mqtt/start_broker.sh" 18887 "$result_dir/broker.log" "$broker_pid_file"
python3 "$repo_dir/tests/mqtt/modbus_tcp_simulator.py" --port 15021 >"$result_dir/modbus.log" 2>&1 &
modbus_pid=$!
sleep 0.3
/tmp/mqmgateway-release-build/modmqttd/modmqttd --config="$script_dir/modbus-load.yaml" >"$result_dir/gateway.log" 2>&1 &
gateway_pid=$!
sleep 0.5
kill -0 "$gateway_pid"
mosquitto_sub -h 127.0.0.1 -p 18887 -W 5 -C 1 \
    -t load/modbus/availability >"$result_dir/availability.txt"
python3 "$script_dir/modbus_gateway_load.py" --pid "$gateway_pid" --duration 5 \
    --output "$repo_dir/results/summary.csv" --raw-output "$result_dir/raw-metrics.json" \
    2>&1 | tee "$result_dir/run.log"

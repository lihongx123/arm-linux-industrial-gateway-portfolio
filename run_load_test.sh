#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
result_dir="$repo_dir/results/load"
summary_csv="$repo_dir/results/summary.csv"
gateway_bin="${IOT_GATEWAY_BIN:-/tmp/mqmgateway-release-build/src/iot_gateway/mqmgateway_iot}"
broker_port=18886
broker_pid_file="$result_dir/broker.pid"
mkdir -p "$result_dir"
ip link show vcan0 >/dev/null

cleanup() {
    [[ -z "${gateway_pid:-}" ]] || kill "$gateway_pid" 2>/dev/null || true
    if [[ -s "$broker_pid_file" ]]; then kill "$(<"$broker_pid_file")" 2>/dev/null || true; fi
}
trap cleanup EXIT INT TERM

bash "$repo_dir/tests/mqtt/start_broker.sh" "$broker_port" "$result_dir/broker.log" "$broker_pid_file"

run_scenario() {
    local scenario="$1" devices="$2" rate="$3" duration="$4"
    local scenario_dir="$result_dir/$scenario"
    mkdir -p "$scenario_dir"
    "$gateway_bin" --mqtt-port="$broker_port" --client-id="load-$scenario" \
        --can-interface=vcan0 --queue-capacity=8192 --workers=4 --heartbeat-ms=200 \
        --metrics-file="$scenario_dir/gateway-metrics.json" >"$scenario_dir/gateway.log" 2>&1 &
    gateway_pid=$!
    mosquitto_sub -h 127.0.0.1 -p "$broker_port" -W 10 -C 1 \
        -t "gateway/load-$scenario/status" >"$scenario_dir/online.json"
    python3 "$repo_dir/tests/load/can_mqtt_load.py" \
        --scenario "$scenario" --devices "$devices" --rate "$rate" --duration "$duration" \
        --broker-port "$broker_port" --gateway-pid "$gateway_pid" \
        --metrics-file "$scenario_dir/gateway-metrics.json" \
        --output "$summary_csv" --raw-output "$scenario_dir/raw-metrics.json" \
        2>&1 | tee "$scenario_dir/run.log"
    kill -TERM "$gateway_pid"
    wait "$gateway_pid"
    gateway_pid=
}

if [[ "${1:-}" == "--stability-only" ]]; then
    run_scenario J_stability_60s 100 500 60
else
    run_scenario A_normal_1_device 1 100 5
    run_scenario B_10_devices 10 500 5
    run_scenario C_50_devices 50 1000 5
    run_scenario D_100_devices 100 1500 5
    run_scenario E_burst 100 5000 2
fi

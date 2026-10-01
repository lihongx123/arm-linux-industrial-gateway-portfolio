#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/../.." && pwd)"
result_dir="${RESULT_DIR:-$repo_dir/results/functional/modbus}"
gateway_link=/tmp/mqmgateway-rtu-gateway
simulator_link=/tmp/mqmgateway-rtu-simulator
fault_file=/tmp/mqmgateway-rtu-fault

mkdir -p "$result_dir"

cleanup() {
    [[ -z "${simulator_pid:-}" ]] || kill "$simulator_pid" 2>/dev/null || true
    [[ -z "${socat_pid:-}" ]] || kill "$socat_pid" 2>/dev/null || true
    rm -f "$gateway_link" "$simulator_link" "$fault_file"
}
trap cleanup EXIT INT TERM

pair_info="$(bash "$script_dir/start_virtual_serial.sh" "$gateway_link" "$simulator_link" "$result_dir/socat.log")"
socat_pid="$(sed -n 's/^socat_pid=\([0-9]*\).*/\1/p' <<<"$pair_info")"
[[ -n "$socat_pid" ]]

python3 "$script_dir/rtu_simulator.py" \
    --device "$simulator_link" \
    --fault-file "$fault_file" \
    --metrics "$result_dir/rtu-frames.csv" >"$result_dir/simulator.log" 2>&1 &
simulator_pid=$!
sleep 0.3
kill -0 "$simulator_pid"

python3 "$script_dir/rtu_client_test.py" \
    --device "$gateway_link" \
    --fault-file "$fault_file" \
    --summary "$result_dir/summary.json" 2>&1 | tee "$result_dir/client.log"

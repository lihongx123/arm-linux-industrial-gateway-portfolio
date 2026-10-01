#!/usr/bin/env bash
set -euo pipefail

script_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
repo_dir="$(cd "$script_dir/../.." && pwd)"
result_dir="${RESULT_DIR:-$repo_dir/results/functional/mqtt}"
gateway_bin="${GATEWAY_BIN:-/tmp/mqmgateway-release-build/modmqttd/modmqttd}"
message_count="${MESSAGE_COUNT:-20}"
broker_port=18883
modbus_port=15020

mkdir -p "$result_dir"
broker_log="$result_dir/broker.log"
broker_pid_file="$result_dir/broker.pid"
modbus_log="$result_dir/modbus-tcp.log"
gateway_log="$result_dir/gateway.log"
subscriber_log="$result_dir/subscriber.log"
metrics_csv="$result_dir/roundtrip.csv"
summary_file="$result_dir/summary.txt"

cleanup() {
    for pid_name in subscriber_pid gateway_pid modbus_pid; do
        pid="${!pid_name:-}"
        [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true
    done
    if [[ -s "$broker_pid_file" ]]; then
        kill "$(<"$broker_pid_file")" 2>/dev/null || true
    fi
}
trap cleanup EXIT INT TERM

"$script_dir/start_broker.sh" "$broker_port" "$broker_log" "$broker_pid_file"
python3 "$script_dir/modbus_tcp_simulator.py" --port "$modbus_port" >"$modbus_log" 2>&1 &
modbus_pid=$!

for _ in {1..50}; do
    if python3 - "$modbus_port" <<'PY'
import socket, sys
s = socket.socket()
s.settimeout(0.1)
try:
    s.connect(("127.0.0.1", int(sys.argv[1])))
except OSError:
    raise SystemExit(1)
finally:
    s.close()
PY
    then
        break
    fi
    sleep 0.1
done
kill -0 "$modbus_pid"

"$gateway_bin" --config="$script_dir/gateway-roundtrip.yaml" >"$gateway_log" 2>&1 &
gateway_pid=$!

for _ in {1..100}; do
    if grep -q "Ready to process MQTT messages" "$gateway_log"; then
        break
    fi
    kill -0 "$gateway_pid"
    sleep 0.1
done
grep -q "Ready to process MQTT messages" "$gateway_log"

stdbuf -oL mosquitto_sub -h 127.0.0.1 -p "$broker_port" -q 1 \
    -t device/phase3/state -F '%p' >"$subscriber_log" 2>&1 &
subscriber_pid=$!
sleep 0.2

printf 'sequence,start_ns,end_ns,latency_ms,value,status\n' >"$metrics_csv"
success=0
failed=0

for ((i = 1; i <= message_count; i++)); do
    value=$((1000 + i))
    start_ns="$(date +%s%N)"
    mosquitto_pub -h 127.0.0.1 -p "$broker_port" -q 1 \
        -t device/phase3/set -m "$value"

    matched=false
    for _ in {1..100}; do
        if grep -qx "$value" "$subscriber_log"; then
            matched=true
            break
        fi
        sleep 0.02
    done
    end_ns="$(date +%s%N)"
    latency_ms="$(awk -v s="$start_ns" -v e="$end_ns" 'BEGIN {printf "%.3f", (e-s)/1000000}')"

    if [[ "$matched" == true ]]; then
        status=pass
        success=$((success + 1))
    else
        status=fail
        failed=$((failed + 1))
    fi
    printf '%d,%s,%s,%s,%d,%s\n' "$i" "$start_ns" "$end_ns" "$latency_ms" "$value" "$status" >>"$metrics_csv"
done

python3 - "$metrics_csv" "$summary_file" "$message_count" "$success" "$failed" <<'PY'
import csv, datetime, statistics, sys

metrics_path, summary_path, total, success, failed = sys.argv[1:]
with open(metrics_path, newline="", encoding="utf-8") as handle:
    passed = [float(row["latency_ms"]) for row in csv.DictReader(handle) if row["status"] == "pass"]

def percentile(values, p):
    if not values:
        return float("nan")
    values = sorted(values)
    index = (len(values) - 1) * p
    lower = int(index)
    upper = min(lower + 1, len(values) - 1)
    return values[lower] + (values[upper] - values[lower]) * (index - lower)

lines = [
    f"timestamp_utc={datetime.datetime.now(datetime.timezone.utc).isoformat()}",
    f"message_count={total}",
    f"success_count={success}",
    f"failed_count={failed}",
]
if passed:
    lines += [
        f"latency_min_ms={min(passed):.3f}",
        f"latency_mean_ms={statistics.fmean(passed):.3f}",
        f"latency_p50_ms={percentile(passed, 0.50):.3f}",
        f"latency_p95_ms={percentile(passed, 0.95):.3f}",
        f"latency_p99_ms={percentile(passed, 0.99):.3f}",
        f"latency_max_ms={max(passed):.3f}",
    ]
with open(summary_path, "w", encoding="utf-8") as handle:
    handle.write("\n".join(lines) + "\n")
print("\n".join(lines))
PY

[[ "$failed" -eq 0 ]]

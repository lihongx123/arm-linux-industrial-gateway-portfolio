#!/usr/bin/env bash
set -euo pipefail

port="${1:-18883}"
log_file="${2:-/tmp/mqmgateway-mosquitto.log}"
pid_file="${3:-/tmp/mqmgateway-mosquitto.pid}"

if [[ -s "$pid_file" ]] && kill -0 "$(<"$pid_file")" 2>/dev/null; then
    echo "broker already running: pid=$(<"$pid_file") port=$port"
    exit 0
fi

mosquitto -p "$port" -v >"$log_file" 2>&1 &
broker_pid=$!
echo "$broker_pid" >"$pid_file"

for _ in {1..50}; do
    if mosquitto_pub -h 127.0.0.1 -p "$port" -t mqmgateway/health -m ready 2>/dev/null; then
        echo "broker ready: pid=$broker_pid port=$port log=$log_file"
        exit 0
    fi
    sleep 0.1
done

kill "$broker_pid" 2>/dev/null || true
echo "broker failed to become ready; see $log_file" >&2
exit 1

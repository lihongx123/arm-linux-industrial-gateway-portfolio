#!/bin/sh
set -eu

duration_seconds="${1:-28800}"
rate="${2:-500}"
devices="${3:-100}"
queue_capacity="${4:-1024}"
workers="${5:-2}"
result_dir=/root/soak-results
mkdir -p "$result_dir"
rm -f "$result_dir"/* /tmp/soak-mosquitto.log /tmp/soak-gateway.log /tmp/soak-metrics.json

cleanup() {
    kill "${gateway_pid:-}" "${broker_pid:-}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

if ! ip link show vcan0 >/dev/null 2>&1; then
    ip link add dev vcan0 type vcan
fi
ip link set up vcan0

killall mosquitto 2>/dev/null || true
sleep 2
mosquitto -c /root/mosquitto.conf >/tmp/soak-mosquitto.log 2>&1 & broker_pid=$!
sleep 1
kill -0 "$broker_pid"

mqmgateway_iot --mqtt-host=127.0.0.1 --mqtt-port=1883 --client-id=arm64-soak \
    --can-interface=vcan0 --queue-capacity="$queue_capacity" --workers="$workers" --heartbeat-ms=1000 \
    --metrics-file=/tmp/soak-metrics.json >/tmp/soak-gateway.log 2>&1 & gateway_pid=$!
mosquitto_sub -h 127.0.0.1 -W 15 -C 1 -t gateway/arm64-soak/status > "$result_dir/online.json"
sleep 1

set +e
mqmgateway_arm64_stress --rate="$rate" --duration="$duration_seconds" --devices="$devices" \
    --gateway-pid="$gateway_pid" --metrics-file=/tmp/soak-metrics.json \
    --output="$result_dir/soak-result.json" > "$result_dir/probe.log" 2>&1
probe_rc=$?
set -e

kill -TERM "$gateway_pid" 2>/dev/null || true
wait "$gateway_pid" 2>/dev/null || true
gateway_pid=
cp /tmp/soak-metrics.json "$result_dir/gateway-metrics.json" 2>/dev/null || true
cp /tmp/soak-gateway.log "$result_dir/gateway.log"
cp /tmp/soak-mosquitto.log "$result_dir/mosquitto.log"
printf 'duration_seconds=%s\nrate=%s\ndevices=%s\nqueue_capacity=%s\nworkers=%s\nprobe_exit=%s\n' \
    "$duration_seconds" "$rate" "$devices" "$queue_capacity" "$workers" "$probe_rc" \
    > "$result_dir/run-parameters.txt"
exit "$probe_rc"

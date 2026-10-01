#!/bin/sh
set -eu

result_dir=/root/stress-results
duration="${1:-30}"
devices="${2:-100}"
queue_capacity="${3:-1024}"
workers="${4:-2}"
mkdir -p "$result_dir"
rm -rf "$result_dir"/*

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
mosquitto -c /root/mosquitto.conf >/tmp/stress-mosquitto.log 2>&1 & broker_pid=$!
sleep 1
kill -0 "$broker_pid"

printf 'rate,classification,probe_exit\n' > "$result_dir/index.csv"
last_stable=0
first_fail=0
case_number=0

run_case() {
    rate="$1"
    label="$2"
    case_duration="$3"
    case_number=$((case_number + 1))
    case_dir="$result_dir/$(printf '%02d' "$case_number")-$label"
    mkdir -p "$case_dir"
    metrics=/tmp/stress-metrics.json
    rm -f "$metrics" /tmp/stress-gateway.log
    client_id="stress-$case_number-$rate"
    mqmgateway_iot --mqtt-host=127.0.0.1 --mqtt-port=1883 --client-id="$client_id" \
        --can-interface=vcan0 --queue-capacity="$queue_capacity" --workers="$workers" \
        --heartbeat-ms=1000 --metrics-file="$metrics" >/tmp/stress-gateway.log 2>&1 & gateway_pid=$!
    mosquitto_sub -h 127.0.0.1 -W 15 -C 1 -t "gateway/$client_id/status" > "$case_dir/online.json"
    sleep 1
    set +e
    mqmgateway_arm64_stress --rate="$rate" --duration="$case_duration" --devices="$devices" \
        --gateway-pid="$gateway_pid" --metrics-file="$metrics" --output="$case_dir/result.json" \
        > "$case_dir/probe.log" 2>&1
    case_rc=$?
    set -e
    kill -TERM "$gateway_pid" 2>/dev/null || true
    wait "$gateway_pid" 2>/dev/null || true
    gateway_pid=
    cp "$metrics" "$case_dir/gateway-metrics.json" 2>/dev/null || true
    cp /tmp/stress-gateway.log "$case_dir/gateway.log"
    if [ -f "$case_dir/result.json" ]; then
        classification="$(sed -n 's/.*"classification":"\([A-Z]*\)".*/\1/p' "$case_dir/result.json")"
    else
        classification=HARNESS_ERROR
    fi
    printf '%s,%s,%s\n' "$rate" "$classification" "$case_rc" >> "$result_dir/index.csv"
    printf '%s rate=%s result=%s rc=%s\n' "$label" "$rate" "$classification" "$case_rc"
}

for rate in 100 250 500 1000 1500 2000 3000 5000 7500 10000 15000 20000 30000 50000 100000; do
    run_case "$rate" "rate-$rate" "$duration"
    if [ "$case_rc" -eq 0 ]; then
        last_stable="$rate"
    elif [ "$case_rc" -eq 3 ]; then
        first_fail="$rate"
        break
    elif [ "$case_rc" -eq 4 ]; then
        printf 'HARNESS_ERROR at rate %s\n' "$rate" > "$result_dir/final-status.txt"
        exit 4
    fi
done

if [ "$first_fail" -gt 0 ] && [ "$last_stable" -gt 0 ]; then
    boundary_rate=$(((last_stable + first_fail) / 2))
    run_case "$boundary_rate" "boundary-${boundary_rate}-a" 60
    run_case "$boundary_rate" "boundary-${boundary_rate}-b" 60
fi

printf 'last_stable_rate=%s\nfirst_fail_rate=%s\nworkers=%s\nqueue_capacity=%s\ndevices=%s\n' \
    "$last_stable" "$first_fail" "$workers" "$queue_capacity" "$devices" \
    | tee "$result_dir/final-status.txt"

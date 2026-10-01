#!/bin/sh
set -eux

result_dir=/root/results
mkdir -p "$result_dir"
rm -f "$result_dir"/* /tmp/mosquitto.log /tmp/gateway.log /tmp/metrics.json

cleanup() {
    kill "${gateway_pid:-}" "${broker_pid:-}" "${subscriber_pid:-}" "${candump_pid:-}" 2>/dev/null || true
}
trap cleanup EXIT INT TERM

start_broker() {
    killall mosquitto 2>/dev/null || true
    sleep 2
    mosquitto -c /root/mosquitto.conf >/tmp/mosquitto.log 2>&1 &
    broker_pid=$!
    sleep 1
    if ! kill -0 "$broker_pid"; then
        cat /tmp/mosquitto.log
        return 1
    fi
}

echo "machine=$(uname -m)" | tee "$result_dir/environment.txt"
uname -a >> "$result_dir/environment.txt"
/usr/bin/modmqttd --version >> "$result_dir/environment.txt"
/usr/bin/mqmgateway_rtu_transport_tests | tee "$result_dir/rtu-stream.json"

ip link add dev vcan0 type vcan
ip link set up vcan0
start_broker

mqmgateway_iot --mqtt-host=127.0.0.1 --mqtt-port=1883 --client-id=arm64-qemu \
    --can-interface=vcan0 --queue-capacity=256 --workers=2 --heartbeat-ms=200 \
    --metrics-file=/tmp/metrics.json >/tmp/gateway.log 2>&1 &
gateway_pid=$!
mosquitto_sub -h 127.0.0.1 -W 15 -C 1 -t gateway/arm64-qemu/status > "$result_dir/online.json"

mosquitto_sub -h 127.0.0.1 -W 15 -C 1 -t device/can-291/telemetry > "$result_dir/can-to-mqtt.json" &
subscriber_pid=$!
sleep 1
cansend vcan0 123#01020304
wait "$subscriber_pid"
subscriber_pid=
grep -q '"address":291' "$result_dir/can-to-mqtt.json"

candump -n 1 vcan0 > "$result_dir/mqtt-to-can.txt" &
candump_pid=$!
sleep 1
mosquitto_pub -h 127.0.0.1 -q 1 -t device/can0/cmd/can_tx \
    -m '{"can_id":1110,"data":"deadbeef","timeout_ms":1000}'
wait "$candump_pid"
candump_pid=
grep -qi '456.*DE AD BE EF\|456#DEADBEEF' "$result_dir/mqtt-to-can.txt"

mosquitto_sub -h 127.0.0.1 -W 15 -C 1 -t device/can0/status > "$result_dir/invalid-command.json" &
subscriber_pid=$!
sleep 1
mosquitto_pub -h 127.0.0.1 -q 1 -t device/can0/cmd/can_tx -m '{"can_id":1,"data":"not-hex"}'
wait "$subscriber_pid"
subscriber_pid=
grep -q '"status":"rejected"' "$result_dir/invalid-command.json"

# Broker outage and reconnect: data enters the bounded queue while MQTT is absent.
kill "$broker_pid"
wait "$broker_pid" || true
broker_pid=
for id in 101 102 103 104 105 106 107 108 109 110; do cansend vcan0 "$id#0102030405060708"; done
sleep 2
start_broker
mosquitto_sub -h 127.0.0.1 -W 20 -C 1 -t gateway/arm64-qemu/status > "$result_dir/reconnected.json"
grep -q '"status":"online"' "$result_dir/reconnected.json"

# QEMU performance run: 500 CAN frames at a nominal 500 frames/s.
mosquitto_sub -h 127.0.0.1 -W 30 -C 500 -t 'device/+/telemetry' > "$result_dir/performance-messages.jsonl" &
subscriber_pid=$!
sleep 1
start_ticks="$(cut -d. -f1 /proc/uptime)"
cangen vcan0 -g 2 -n 500 -I i -L 8
wait "$subscriber_pid"
subscriber_pid=
end_ticks="$(cut -d. -f1 /proc/uptime)"
received="$(wc -l < "$result_dir/performance-messages.jsonl")"
elapsed="$((end_ticks - start_ticks))"
[ "$received" -eq 500 ]

kill -TERM "$gateway_pid"
wait "$gateway_pid"
gateway_pid=
cp /tmp/metrics.json "$result_dir/gateway-metrics.json"
cp /tmp/gateway.log "$result_dir/gateway.log"
cp /tmp/mosquitto.log "$result_dir/mosquitto.log"
printf '{"status":"PASS","architecture":"%s","rtu_stream":"PASS","can_to_mqtt":"PASS","mqtt_to_can":"PASS","invalid_command":"PASS","broker_reconnect":"PASS","performance_sent":500,"performance_received":%s,"loss":0,"elapsed_seconds":%s}\n' \
    "$(uname -m)" "$received" "$elapsed" | tee "$result_dir/summary.json"

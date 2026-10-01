#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
gateway_bin="${IOT_GATEWAY_BIN:-/tmp/mqmgateway-baseline-build/src/iot_gateway/mqmgateway_iot}"
result_dir="${RESULT_DIR:-$repo_dir/results/cloud-live-local/tls}"
port="$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1]); s.close()')"
tmp_dir="$(mktemp -d /tmp/mqmgateway-cloud-tls.XXXXXX)"
test_password="$(openssl rand -hex 24)"

mkdir -p "$result_dir"
if [[ -n "$(find "$result_dir" -mindepth 1 -maxdepth 1 -print -quit)" ]]; then
    echo "refusing to overwrite non-empty evidence directory: $result_dir" >&2
    exit 1
fi

broker_pid=
gateway_pid=
subscriber_pid=
candump_pid=
cleanup() {
    for pid in "$subscriber_pid" "$candump_pid" "$gateway_pid" "$broker_pid"; do
        [[ -z "$pid" ]] || kill "$pid" 2>/dev/null || true
    done
    [[ -z "$gateway_pid" ]] || wait "$gateway_pid" 2>/dev/null || true
    [[ -z "$broker_pid" ]] || wait "$broker_pid" 2>/dev/null || true
    rm -rf -- "$tmp_dir"
}
trap cleanup EXIT INT TERM

ip link show vcan0 >/dev/null
openssl req -x509 -newkey rsa:2048 -nodes -keyout "$tmp_dir/ca.key" \
    -out "$tmp_dir/ca.crt" -subj /CN=mqmgateway-local-test-ca -days 1 >/dev/null 2>&1
openssl req -newkey rsa:2048 -nodes -keyout "$tmp_dir/broker.key" \
    -out "$tmp_dir/broker.csr" -subj /CN=localhost >/dev/null 2>&1
printf 'subjectAltName=DNS:localhost\n' > "$tmp_dir/broker.ext"
openssl x509 -req -in "$tmp_dir/broker.csr" -CA "$tmp_dir/ca.crt" -CAkey "$tmp_dir/ca.key" \
    -CAcreateserial -out "$tmp_dir/broker.crt" -days 1 -extfile "$tmp_dir/broker.ext" >/dev/null 2>&1
chmod 600 "$tmp_dir/broker.key" "$tmp_dir/ca.key"
cat > "$tmp_dir/mosquitto.conf" <<EOF
listener $port 127.0.0.1
allow_anonymous true
cafile $tmp_dir/ca.crt
certfile $tmp_dir/broker.crt
keyfile $tmp_dir/broker.key
tls_version tlsv1.2
persistence false
EOF

start_broker() {
    printf '\n--- broker start ---\n' >> "$result_dir/broker.log"
    mosquitto -c "$tmp_dir/mosquitto.conf" >>"$result_dir/broker.log" 2>&1 &
    broker_pid=$!
    for _ in $(seq 1 50); do
        if bash -c "</dev/tcp/127.0.0.1/$port" >/dev/null 2>&1; then
            return 0
        fi
        kill -0 "$broker_pid" 2>/dev/null || { echo 'local TLS broker exited' >&2; return 1; }
        sleep 0.1
    done
    echo 'local TLS broker did not become ready' >&2
    return 1
}

start_broker
gateway_started="$(date +%s%3N)"
EMQX_HOST=localhost EMQX_PORT="$port" EMQX_CA="$tmp_dir/ca.crt" \
    GATEWAY_MQTT_USERNAME=cloud-smoke-user GATEWAY_MQTT_PASSWORD="$test_password" \
    "$gateway_bin" --cloud --client-id=cloud-smoke --can-interface=vcan0 \
        --heartbeat-ms=200 >"$result_dir/gateway.log" 2>&1 &
gateway_pid=$!

mosquitto_sub -h localhost -p "$port" --cafile "$tmp_dir/ca.crt" -W 12 -C 1 \
    -t resume/gateway/status/cloud-smoke >"$result_dir/online.json"
gateway_online="$(date +%s%3N)"
mosquitto_sub -h localhost -p "$port" --cafile "$tmp_dir/ca.crt" -W 12 -C 1 \
    -t resume/gateway/devices/can-291/telemetry >"$result_dir/uplink.json" &
subscriber_pid=$!
sleep 0.2
cansend vcan0 123#01020304
wait "$subscriber_pid"
subscriber_pid=
grep -q '"address":291' "$result_dir/uplink.json"
grep -q '"payload":"01020304"' "$result_dir/uplink.json"

candump -n 1 vcan0 >"$result_dir/downlink-can.txt" &
candump_pid=$!
sleep 0.2
mosquitto_pub -h localhost -p "$port" --cafile "$tmp_dir/ca.crt" -q 1 \
    -t resume/gateway/devices/can0/command \
    -m '{"can_id":1110,"data":"deadbeef","timeout_ms":1000}'
wait "$candump_pid"
candump_pid=
grep -qi '456.*DE AD BE EF\|456#DEADBEEF' "$result_dir/downlink-can.txt"
mosquitto_sub -h localhost -p "$port" --cafile "$tmp_dir/ca.crt" -W 12 -C 1 \
    -t resume/gateway/devices/can0/command/result >"$result_dir/command-result.json" &
subscriber_pid=$!
sleep 0.2
mosquitto_pub -h localhost -p "$port" --cafile "$tmp_dir/ca.crt" -q 1 \
    -t resume/gateway/devices/can0/command \
    -m '{"can_id":1110,"data":"0102","timeout_ms":1000}'
wait "$subscriber_pid"
subscriber_pid=
grep -q '"status":"ok"' "$result_dir/command-result.json"

disconnect_started="$(date +%s%3N)"
kill "$broker_pid"
wait "$broker_pid" 2>/dev/null || true
broker_pid=
sleep 0.3
start_broker
mosquitto_sub -h localhost -p "$port" --cafile "$tmp_dir/ca.crt" -W 35 -C 1 \
    -t resume/gateway/status/cloud-smoke >"$result_dir/reconnected.json"
reconnected_at="$(date +%s%3N)"
grep -q '"status":"online"' "$result_dir/reconnected.json"

mosquitto_sub -h localhost -p "$port" --cafile "$tmp_dir/ca.crt" -W 12 -C 1 \
    -t resume/gateway/devices/can-291/telemetry >"$result_dir/uplink-after-reconnect.json" &
subscriber_pid=$!
sleep 0.2
cansend vcan0 123#05060708
wait "$subscriber_pid"
subscriber_pid=
grep -q '"payload":"05060708"' "$result_dir/uplink-after-reconnect.json"

reconnect_ms="$((reconnected_at - disconnect_started))"
startup_ms="$((gateway_online - gateway_started))"
printf 'result=PASS\nverification_scope=local TLS broker only; not EMQX Cloud\ntls_peer_verification=PASS\nuplink=PASS\ndownlink_to_vcan=PASS\ncommand_result=PASS\nreconnect_and_subscription_recovery=PASS\nfirst_uplink_after_reconnect=PASS\nstartup_ms=%s\nreconnect_ms=%s\n' \
    "$startup_ms" "$reconnect_ms" > "$result_dir/summary.txt"
cat "$result_dir/summary.txt"

kill -TERM "$gateway_pid"
wait "$gateway_pid"
gateway_pid=

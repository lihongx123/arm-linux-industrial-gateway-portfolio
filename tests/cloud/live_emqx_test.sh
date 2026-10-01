#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
result_dir="${RESULT_DIR:-$repo_dir/results/cloud-live-emqx}"
gateway_bin="${IOT_GATEWAY_BIN:-/tmp/mqmgateway-cloud-live-native/src/iot_gateway/mqmgateway_iot}"
secret_file="${MQMGATEWAY_EMQX_ENV:-${XDG_CONFIG_HOME:-$HOME/.config}/mqmgateway/emqx.env}"
device_id=gateway-demo-001
telemetry_device=can-291
client_id=gateway-project-a-live

gateway_pid=
subscriber_pid=
candump_pid=
gateway_stopped=false

cleanup() {
    set +e
    [[ $gateway_stopped != true || -z $gateway_pid ]] || kill -CONT "$gateway_pid" 2>/dev/null
    for process_id in "$subscriber_pid" "$candump_pid" "$gateway_pid"; do
        [[ -z "$process_id" ]] || kill "$process_id" 2>/dev/null
    done
    [[ -z "$gateway_pid" ]] || wait "$gateway_pid" 2>/dev/null
}
trap cleanup EXIT INT TERM

fail() {
    echo "live EMQX test failed: $*" >&2
    exit 1
}

now_ms() {
    date +%s%3N
}

wait_for_log_count() {
    local pattern=$1
    local expected=$2
    local timeout_seconds=$3
    local deadline=$((SECONDS + timeout_seconds))
    while ((SECONDS < deadline)); do
        if [[ $(grep -cF "$pattern" "$result_dir/gateway.log" 2>/dev/null || true) -ge $expected ]]; then
            return 0
        fi
        sleep 0.05
    done
    return 1
}

[[ $EUID -eq 0 ]] || fail "run as root so reversible host routes can inject the network fault"
[[ -f "$secret_file" ]] || fail "private environment file is missing"
[[ $(stat -c %a "$secret_file") == 600 ]] || fail "private environment file permissions must be 600"
set -a
source "$secret_file"
set +a

[[ ${EMQX_HOST:-} == kf7bd96b.ala.dedicated.aliyun.emqxcloud.cn ]] || fail "unexpected EMQX_HOST"
[[ ${EMQX_PORT:-} == 8883 ]] || fail "unexpected EMQX_PORT"
[[ -n ${EMQX_CA:-} && -r $EMQX_CA ]] || fail "EMQX_CA must point to a readable CA certificate"
[[ ${GATEWAY_MQTT_USERNAME:-} == gateway-client ]] || fail "unexpected gateway username"
[[ -n ${GATEWAY_MQTT_PASSWORD:-} ]] || fail "gateway password is missing"
[[ -x $gateway_bin ]] || fail "gateway binary is not executable"
ip link show vcan0 >/dev/null || fail "vcan0 is unavailable"

credential_marker="export GATEWAY_MQTT_PASSWORD='$GATEWAY_MQTT_PASSWORD'"
credential_matches=$(grep -RIlF --exclude-dir=.git -- "$credential_marker" "$repo_dir" || true)
if [[ -n $credential_matches ]]; then
    printf 'credential match: %s\n' "$credential_matches" >&2
    fail "gateway password was found in repository files"
fi
if [[ ${CREDENTIAL_SCAN_ONLY:-0} == 1 ]]; then
    echo "credential scan: PASS"
    exit 0
fi
mkdir -p "$result_dir"

mqtt_options=(
    -h "$EMQX_HOST"
    -p "$EMQX_PORT"
    --cafile "$EMQX_CA"
    -u "$GATEWAY_MQTT_USERNAME"
    -P "$GATEWAY_MQTT_PASSWORD"
)

# Independent MQTT authentication, QoS 1 publish, subscribe, and receive.
auth_topic="resume/gateway/devices/$device_id/telemetry"
auth_payload="cloud-auth-$(now_ms)"
auth_receive="$(mktemp /tmp/mqmgateway-auth-receive.XXXXXX)"
mosquitto_sub "${mqtt_options[@]}" -q 1 -t "$auth_topic" -C 1 -W 20 >"$auth_receive" &
subscriber_pid=$!
sleep 0.5
mosquitto_pub "${mqtt_options[@]}" -q 1 -t "$auth_topic" -m "$auth_payload"
wait "$subscriber_pid"
subscriber_pid=
[[ $(<"$auth_receive") == "$auth_payload" ]] || fail "independent MQTT payload mismatch"
rm -f "$auth_receive"
cat >"$result_dir/auth.txt" <<EOF
broker=$EMQX_HOST:$EMQX_PORT
username=$GATEWAY_MQTT_USERNAME
tls=verified CA and hostname
authentication=PASS
subscribe=PASS
publish=PASS
qos=1
independent_receive=PASS
result=PASS
EOF

# Run the repository gateway and wait for an authenticated connection callback.
: >"$result_dir/gateway.log"
EMQX_HOST="$EMQX_HOST" EMQX_PORT="$EMQX_PORT" EMQX_CA="$EMQX_CA" \
    GATEWAY_MQTT_USERNAME="$GATEWAY_MQTT_USERNAME" \
    GATEWAY_MQTT_PASSWORD="$GATEWAY_MQTT_PASSWORD" \
    "$gateway_bin" --cloud --client-id="$client_id" --can-interface=vcan0 \
        --heartbeat-ms=200 --metrics-file="$result_dir/metrics.json" \
        >"$result_dir/gateway.log" 2>&1 &
gateway_pid=$!
wait_for_log_count "MQTT connected result=0" 1 30 || fail "gateway did not connect"

# Uplink: simulated CAN -> gateway -> real EMQX -> independent subscriber.
uplink_receive="$(mktemp /tmp/mqmgateway-uplink-receive.XXXXXX)"
mosquitto_sub "${mqtt_options[@]}" -q 1 \
    -t "resume/gateway/devices/$telemetry_device/telemetry" -C 1 -W 20 >"$uplink_receive" &
subscriber_pid=$!
sleep 0.5
cansend vcan0 123#A1B2C3D4
wait "$subscriber_pid"
subscriber_pid=
grep -q '"device_id":"can-291"' "$uplink_receive" || fail "uplink device_id mismatch"
grep -q '"address":291' "$uplink_receive" || fail "uplink CAN address mismatch"
grep -q '"payload":"a1b2c3d4"' "$uplink_receive" || fail "uplink payload mismatch"
uplink_count=$(wc -l <"$uplink_receive")
{
    printf 'broker=%s:%s\n' "$EMQX_HOST" "$EMQX_PORT"
    printf 'topic=resume/gateway/devices/%s/telemetry\n' "$telemetry_device"
    printf 'input_can=123#A1B2C3D4\n'
    printf 'independent_message_count=%s\n' "$uplink_count"
    printf 'received_payload='
    cat "$uplink_receive"
    printf 'result=PASS\n'
} >"$result_dir/uplink.txt"
rm -f "$uplink_receive"

# Downlink: independent publisher -> EMQX -> router -> simulated CAN, plus result publication.
command_id="cloud-command-$(now_ms)"
downlink_payload="{\"command_id\":\"$command_id\",\"can_id\":1110,\"data\":\"deadbeef\",\"timeout_ms\":5000}"
command_result="$(mktemp /tmp/mqmgateway-command-result.XXXXXX)"
candump -L -n 1 vcan0 >"$result_dir/downlink-can.txt" &
candump_pid=$!
mosquitto_sub "${mqtt_options[@]}" -q 1 \
    -t "resume/gateway/devices/$device_id/command/result" -C 1 -W 20 >"$command_result" &
subscriber_pid=$!
sleep 0.5
mosquitto_pub "${mqtt_options[@]}" -q 1 \
    -t "resume/gateway/devices/$device_id/command" -m "$downlink_payload"
wait "$candump_pid"
candump_pid=
wait "$subscriber_pid"
subscriber_pid=
grep -Eqi '456.*DE AD BE EF|456#DEADBEEF' "$result_dir/downlink-can.txt" || fail "downlink CAN frame mismatch"
grep -q '"status":"ok"' "$command_result" || fail "command result was not successful"
{
    printf 'topic=resume/gateway/devices/%s/command\n' "$device_id"
    printf 'command_id=%s\n' "$command_id"
    printf 'device_id=%s\n' "$device_id"
    printf 'published_payload=%s\n' "$downlink_payload"
    printf 'southbound_frame='
    cat "$result_dir/downlink-can.txt"
    printf 'result=PASS\n'
} >"$result_dir/downlink.txt"
{
    printf 'command_id=%s\n' "$command_id"
    printf 'router=PASS\n'
    printf 'simulated_can_device=PASS\n'
    printf 'command_result_topic=resume/gateway/devices/%s/command/result\n' "$device_id"
    printf 'command_result_payload='
    cat "$command_result"
    printf 'command_result_publication=PASS\n'
} >"$result_dir/command-routing.txt"
rm -f "$command_result"

# Reversible process-level isolation. The independent subscriber remains live
# while the gateway misses more than the broker's 1.5x keepalive window.
pending_uplink="$(mktemp /tmp/mqmgateway-pending-uplink.XXXXXX)"
mosquitto_sub "${mqtt_options[@]}" -q 1 \
    -t "resume/gateway/devices/$telemetry_device/telemetry" -C 1 -W 45 >"$pending_uplink" &
subscriber_pid=$!
sleep 0.5
fault_started=$(now_ms)
kill -STOP "$gateway_pid"
gateway_stopped=true
cansend vcan0 123#11223344
sleep 20
recovery_started=$(now_ms)
kill -CONT "$gateway_pid"
gateway_stopped=false
wait_for_log_count "MQTT disconnected result=" 1 10 || fail "gateway did not detect disconnect"
disconnect_detected=$(now_ms)
wait_for_log_count "MQTT connected result=0" 2 40 || fail "gateway did not reconnect"
reconnected=$(now_ms)
wait "$subscriber_pid"
subscriber_pid=
grep -q '"payload":"11223344"' "$pending_uplink" || fail "outage telemetry was not recovered"
rm -f "$pending_uplink"

# Subscription recovery and first verified command after reconnect.
post_command_result="$(mktemp /tmp/mqmgateway-post-command-result.XXXXXX)"
candump -L -n 1 vcan0 >"$result_dir/post-reconnect-can.txt" &
candump_pid=$!
mosquitto_sub "${mqtt_options[@]}" -q 1 \
    -t "resume/gateway/devices/$device_id/command/result" -C 1 -W 20 >"$post_command_result" &
subscriber_pid=$!
sleep 0.5
post_command_sent=$(now_ms)
mosquitto_pub "${mqtt_options[@]}" -q 1 \
    -t "resume/gateway/devices/$device_id/command" \
    -m '{"command_id":"post-reconnect-command","can_id":1110,"data":"01020304","timeout_ms":5000}'
wait "$candump_pid"
candump_pid=
wait "$subscriber_pid"
subscriber_pid=
post_command_received=$(now_ms)
grep -Eqi '456.*01 02 03 04|456#01020304' "$result_dir/post-reconnect-can.txt" || \
    fail "post-reconnect command did not reach CAN"
grep -q '"status":"ok"' "$post_command_result" || fail "post-reconnect command result failed"
rm -f "$post_command_result"

# First deliberately generated telemetry after reconnect, observed independently.
post_uplink="$(mktemp /tmp/mqmgateway-post-uplink.XXXXXX)"
mosquitto_sub "${mqtt_options[@]}" -q 1 \
    -t "resume/gateway/devices/$telemetry_device/telemetry" -C 1 -W 20 >"$post_uplink" &
subscriber_pid=$!
sleep 0.5
post_telemetry_sent=$(now_ms)
cansend vcan0 123#55667788
wait "$subscriber_pid"
subscriber_pid=
post_telemetry_received=$(now_ms)
grep -q '"payload":"55667788"' "$post_uplink" || fail "post-reconnect telemetry mismatch"
rm -f "$post_uplink"

kill -TERM "$gateway_pid"
wait "$gateway_pid"
gateway_pid=
publish_failures=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["publish_failures"])' \
    "$result_dir/metrics.json")

disconnect_ms=$((disconnect_detected - fault_started))
reconnect_ms=$((reconnected - recovery_started))
post_telemetry_ms=$((post_telemetry_received - post_telemetry_sent))
post_command_ms=$((post_command_received - post_command_sent))
cat >"$result_dir/reconnect.txt" <<EOF
fault_injection=reversible gateway process pause exceeding the real broker keepalive window
disconnect_detection_ms=$disconnect_ms
reconnect_after_network_restore_ms=$reconnect_ms
subscription_recovery=PASS
first_post_reconnect_telemetry_ms=$post_telemetry_ms
first_post_reconnect_command_ms=$post_command_ms
pending_retry_behavior=CAN telemetry injected during isolation was received through EMQX after recovery
final_publish_failures=$publish_failures
result=PASS
EOF

printf 'AUTH=PASS UPLINK=PASS UPLINK_COUNT=%s DOWNLINK=PASS COMMAND_RESULT=PASS RECONNECT=PASS DISCONNECT_MS=%s RECONNECT_MS=%s POST_TELEMETRY_MS=%s POST_COMMAND_MS=%s PUBLISH_FAILURES=%s\n' \
    "$uplink_count" "$disconnect_ms" "$reconnect_ms" "$post_telemetry_ms" "$post_command_ms" "$publish_failures"

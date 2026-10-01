#!/usr/bin/env bash
set -euo pipefail

binary=${1:?gateway binary required}

run_expect() {
    local expected=$1
    shift
    local output rc
    set +e
    output=$(env -u EMQX_HOST -u EMQX_PORT -u EMQX_CA \
        -u GATEWAY_MQTT_USERNAME -u GATEWAY_MQTT_PASSWORD -u GATEWAY_MQTT_TLS \
        "$@" 2>&1)
    rc=$?
    set -e
    if [[ $rc -ne $expected ]]; then
        printf 'unexpected exit status: got %s, expected %s\n' "$rc" "$expected" >&2
        exit 1
    fi
    printf '%s\n' "$output"
}

output=$(run_expect 1 "$binary" --cloud)
grep -q 'cloud mode requires EMQX host and gateway credentials' <<<"$output"

output=$(run_expect 1 "$binary" --mqtt-tls=1)
grep -q 'MQTT TLS requires a CA certificate path' <<<"$output"

output=$(run_expect 2 "$binary" --mqtt-tls=maybe)
grep -q -- '--mqtt-tls must be 0/1 or true/false' <<<"$output"

secret_marker="credential-$(date +%s%N)"
output=$(run_expect 1 env EMQX_HOST=broker.invalid GATEWAY_MQTT_USERNAME=smoke-user \
    GATEWAY_MQTT_PASSWORD="$secret_marker" "$binary" --cloud)
grep -q 'MQTT TLS requires a CA certificate path' <<<"$output"
if grep -qF -- "$secret_marker" <<<"$output"; then
    echo 'credential leaked to diagnostic output' >&2
    exit 1
fi

echo 'Cloud config validation and credential-redaction checks passed'

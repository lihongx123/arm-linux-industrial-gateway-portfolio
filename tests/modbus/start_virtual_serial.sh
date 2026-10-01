#!/usr/bin/env bash
set -euo pipefail

gateway_link="${1:-/tmp/mqmgateway-rtu-gateway}"
simulator_link="${2:-/tmp/mqmgateway-rtu-simulator}"
log_file="${3:-/tmp/mqmgateway-socat.log}"

rm -f "$gateway_link" "$simulator_link"
socat -d -d \
    "PTY,raw,echo=0,link=$gateway_link" \
    "PTY,raw,echo=0,link=$simulator_link" >"$log_file" 2>&1 &
socat_pid=$!

for _ in {1..50}; do
    if [[ -L "$gateway_link" && -L "$simulator_link" ]]; then
        echo "socat_pid=$socat_pid gateway=$gateway_link simulator=$simulator_link"
        exit 0
    fi
    sleep 0.1
done

kill "$socat_pid" 2>/dev/null || true
echo "virtual serial pair failed; see $log_file" >&2
exit 1

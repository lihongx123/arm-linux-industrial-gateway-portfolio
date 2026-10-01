#!/usr/bin/env bash
set -euo pipefail

host="${MQTT_HOST:-127.0.0.1}"
port="${MQTT_PORT:-18883}"
topic="${1:-mqmgateway/test/input}"
count="${2:-10}"
timeout_seconds="${3:-15}"

mosquitto_sub -h "$host" -p "$port" -q 1 -t "$topic" -C "$count" -W "$timeout_seconds"

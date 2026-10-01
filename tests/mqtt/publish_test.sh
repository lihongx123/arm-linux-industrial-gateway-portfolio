#!/usr/bin/env bash
set -euo pipefail

host="${MQTT_HOST:-127.0.0.1}"
port="${MQTT_PORT:-18883}"
topic="${1:-mqmgateway/test/input}"
count="${2:-10}"

for ((i = 1; i <= count; i++)); do
    mosquitto_pub -h "$host" -p "$port" -q 1 -t "$topic" -m "message-$i"
done

echo "published=$count topic=$topic host=$host port=$port"

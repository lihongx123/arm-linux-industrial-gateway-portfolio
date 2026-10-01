#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_dir"
bash tests/modbus/modbus_rtu_test.sh
bash tests/can/can_mqtt_integration_test.sh
bash tests/integration/reliability_test.sh

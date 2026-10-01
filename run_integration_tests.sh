#!/usr/bin/env bash
set -euo pipefail
repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$repo_dir"
cmake --build /tmp/mqmgateway-release-build -j"$(nproc)"
bash tests/mqtt/mqtt_roundtrip_test.sh
bash tests/can/can_mqtt_integration_test.sh
bash tests/integration/reliability_test.sh

#!/usr/bin/env bash
set -euo pipefail

repo_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$repo_dir"
exec python3 scripts/run_buildroot_qemu.py \
    --mode soak \
    --soak-seconds 28800 \
    --soak-rate 500 \
    --soak-devices 100 \
    --soak-queue 1024 \
    --soak-workers 2

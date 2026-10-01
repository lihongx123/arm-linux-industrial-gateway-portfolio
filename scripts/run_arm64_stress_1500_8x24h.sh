#!/bin/sh
set -u

repo_dir="$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)"
run_repo=/tmp/mqmgateway-1500-8x24h-repo-20260922
destination="$repo_dir/results/arm64/stress/long-run-1500-8x24h-20260922"

if [ -e "$run_repo" ] || [ -e "$destination/runner.log" ]; then
    echo "refusing to overwrite existing run path" >&2
    exit 90
fi

mkdir -p "$run_repo" "$destination"
cd "$repo_dir"

python3 scripts/run_buildroot_qemu.py \
    --repo "$run_repo" \
    --output /tmp/mqmgateway-br-output \
    --mode soak \
    --soak-seconds 691200 \
    --soak-rate 1500 \
    --soak-devices 100 \
    --soak-queue 1024 \
    --soak-workers 2 \
    >"$destination/runner.log" 2>&1
run_exit=$?

if [ -d "$run_repo/results/arm64/soak" ]; then
    cp -a "$run_repo/results/arm64/soak/." "$destination/"
fi
printf 'exit_code=%s\n' "$run_exit" > "$destination/exit-status.txt"
exit "$run_exit"

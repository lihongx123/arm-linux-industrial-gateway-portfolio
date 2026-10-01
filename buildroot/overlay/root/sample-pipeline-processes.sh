#!/bin/sh
# Read-only guest /proc snapshots. One CPU core = 100%; not host CPU usage.
gateway_pid="$1"
broker_pid="$2"
probe_pid="$3"
metrics="$4"
printf 'PIDS gateway=%s broker=%s probe=%s\n' "$gateway_pid" "$broker_pid" "$probe_pid"
ticks=$(getconf CLK_TCK 2>/dev/null) || ticks=100
printf 'CLK_TCK=%s (Linux USER_HZ fallback=100 if getconf unavailable)\n' "$ticks"
while kill -0 "$probe_pid" 2>/dev/null; do
    read -r uptime rest < /proc/uptime
    printf 'SAMPLE %s\n' "$uptime"
    for pid in "$gateway_pid" "$broker_pid" "$probe_pid"; do
        [ -r "/proc/$pid/stat" ] || continue
        printf 'PROCESS %s\n' "$pid"
        cat "/proc/$pid/stat"
        grep -E '^(VmRSS|VmHWM|Threads):' "/proc/$pid/status" || true
        for stat in /proc/"$pid"/task/*/stat; do
            [ -r "$stat" ] || continue
            printf 'THREAD '
            cat "$stat"
        done
    done
    printf 'SYSTEM '
    head -n 1 /proc/stat
    grep -E '^(MemAvailable|MemFree|SwapFree):' /proc/meminfo
    printf 'METRICS '
    cat "$metrics" 2>/dev/null || true
    sleep 5
done

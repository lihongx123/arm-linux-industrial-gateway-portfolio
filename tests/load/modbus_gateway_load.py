#!/usr/bin/env python3
import argparse
import csv
import json
import os
import time
from pathlib import Path

import paho.mqtt.client as mqtt


def read_process(pid: int) -> tuple[float, float]:
    fields = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8").split()
    cpu = (int(fields[13]) + int(fields[14])) / os.sysconf(os.sysconf_names["SC_CLK_TCK"])
    status = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    rss = next(int(line.split()[1]) for line in status.splitlines() if line.startswith("VmRSS:")) / 1024
    return cpu, rss


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--duration", type=float, default=5)
    parser.add_argument("--output", required=True)
    parser.add_argument("--raw-output", required=True)
    args = parser.parse_args()
    count = 0

    def on_message(_client, _userdata, _message):
        nonlocal count
        count += 1

    client = mqtt.Client(client_id=f"modbus-load-observer-{os.getpid()}")
    client.on_message = on_message
    client.connect("127.0.0.1", 18887, 10)
    client.subscribe("load/modbus/state", qos=1)
    client.loop_start()
    time.sleep(0.2)
    count = 0
    cpu_start, rss_start = read_process(args.pid)
    start = time.monotonic()
    rss_peak = rss_start
    while time.monotonic() - start < args.duration:
        time.sleep(0.1)
        _, rss = read_process(args.pid)
        rss_peak = max(rss_peak, rss)
    elapsed = time.monotonic() - start
    cpu_end, _ = read_process(args.pid)
    client.loop_stop()
    client.disconnect()
    summary = {
        "scenario": "Modbus_TCP_poll",
        "devices": 1,
        "mqtt_rate": count / elapsed,
        "modbus_rate": count / elapsed,
        "can_rate": 0.0,
        "duration": elapsed,
        "p50_ms": "",
        "p95_ms": "",
        "p99_ms": "",
        "cpu_percent": 100 * (cpu_end - cpu_start) / elapsed,
        "rss_mb": rss_peak,
        "queue_peak": "",
        "error_rate": 0.0,
        "loss_rate": 0.0,
        "reconnect_time_s": "",
    }
    output = Path(args.output)
    write_header = not output.exists() or output.stat().st_size == 0
    with output.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary))
        if write_header:
            writer.writeheader()
        writer.writerow(summary)
    raw = {"timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()), "message_count": count, "summary": summary}
    Path(args.raw_output).write_text(json.dumps(raw, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(raw, indent=2))


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Generate timestamped SocketCAN traffic and measure MQTT delivery."""

import argparse
import csv
import json
import math
import os
import socket
import statistics
import struct
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt


def percentile(values: list[float], fraction: float) -> float:
    if not values:
        return math.nan
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    lower = int(position)
    upper = min(lower + 1, len(ordered) - 1)
    return ordered[lower] + (ordered[upper] - ordered[lower]) * (position - lower)


def read_process(pid: int) -> tuple[float, float]:
    stat = Path(f"/proc/{pid}/stat").read_text(encoding="utf-8").split()
    ticks = int(stat[13]) + int(stat[14])
    status = Path(f"/proc/{pid}/status").read_text(encoding="utf-8")
    rss_kb = next(int(line.split()[1]) for line in status.splitlines() if line.startswith("VmRSS:"))
    return ticks / os.sysconf(os.sysconf_names["SC_CLK_TCK"]), rss_kb / 1024


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--scenario", required=True)
    parser.add_argument("--devices", type=int, required=True)
    parser.add_argument("--rate", type=float, required=True)
    parser.add_argument("--duration", type=float, required=True)
    parser.add_argument("--broker-port", type=int, required=True)
    parser.add_argument("--gateway-pid", type=int, required=True)
    parser.add_argument("--metrics-file", required=True)
    parser.add_argument("--output", required=True)
    parser.add_argument("--raw-output", required=True)
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()

    latencies: list[float] = []
    received = 0
    malformed = 0
    lock = threading.Lock()
    connected = threading.Event()

    def on_connect(client, _userdata, _flags, result):
        if result == 0:
            client.subscribe("device/+/telemetry", qos=1)
            connected.set()

    def on_message(_client, _userdata, message):
        nonlocal received, malformed
        try:
            body = json.loads(message.payload)
            payload = bytes.fromhex(body["payload"])
            sent_ns = int.from_bytes(payload, "big")
            latency_ms = (time.time_ns() - sent_ns) / 1_000_000
            if latency_ms < 0 or latency_ms > 60_000:
                raise ValueError("timestamp outside measurement range")
            with lock:
                received += 1
                latencies.append(latency_ms)
        except (KeyError, ValueError, TypeError, json.JSONDecodeError):
            with lock:
                malformed += 1

    client = mqtt.Client(client_id=f"load-{args.scenario}-{os.getpid()}", clean_session=True)
    client.on_connect = on_connect
    client.on_message = on_message
    client.connect("127.0.0.1", args.broker_port, keepalive=10)
    client.loop_start()
    if not connected.wait(5):
        raise RuntimeError("MQTT subscriber failed to connect")

    can_socket = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    can_socket.bind((args.interface,))

    cpu_start, _ = read_process(args.gateway_pid)
    wall_start = time.monotonic()
    end_at = wall_start + args.duration
    next_send = wall_start
    sent = 0
    rss_samples: list[float] = []
    interval = 1.0 / args.rate

    while time.monotonic() < end_at:
        now = time.monotonic()
        if now < next_send:
            time.sleep(min(next_send - now, 0.001))
            continue
        device = sent % args.devices + 1
        timestamp = time.time_ns().to_bytes(8, "big")
        frame = struct.pack("=IB3x8s", device, 8, timestamp)
        can_socket.send(frame)
        sent += 1
        next_send += interval
        if sent % max(1, int(args.rate / 5)) == 0:
            try:
                _, rss = read_process(args.gateway_pid)
                rss_samples.append(rss)
            except FileNotFoundError:
                pass

    generation_end = time.monotonic()
    deadline = generation_end + 2.0
    while time.monotonic() < deadline:
        with lock:
            if received >= sent:
                break
        time.sleep(0.01)
    measurement_end = time.monotonic()
    cpu_end, rss_end = read_process(args.gateway_pid)
    rss_samples.append(rss_end)
    client.loop_stop()
    client.disconnect()
    can_socket.close()

    with lock:
        latency_snapshot = list(latencies)
        received_snapshot = received
        malformed_snapshot = malformed
    generation_duration = generation_end - wall_start
    cpu_percent = 100 * (cpu_end - cpu_start) / (measurement_end - wall_start)
    try:
        gateway_metrics = json.loads(Path(args.metrics_file).read_text(encoding="utf-8"))
    except (FileNotFoundError, json.JSONDecodeError):
        gateway_metrics = {}

    lost = max(0, sent - received_snapshot)
    summary = {
        "scenario": args.scenario,
        "devices": args.devices,
        "mqtt_rate": received_snapshot / generation_duration,
        "modbus_rate": 0.0,
        "can_rate": sent / generation_duration,
        "duration": generation_duration,
        "p50_ms": percentile(latency_snapshot, 0.50),
        "p95_ms": percentile(latency_snapshot, 0.95),
        "p99_ms": percentile(latency_snapshot, 0.99),
        "cpu_percent": cpu_percent,
        "rss_mb": max(rss_samples) if rss_samples else math.nan,
        "queue_peak": gateway_metrics.get("peak_depth", 0),
        "error_rate": malformed_snapshot / sent if sent else 0.0,
        "loss_rate": lost / sent if sent else 0.0,
        "reconnect_time_s": "",
    }
    output_path = Path(args.output)
    output_path.parent.mkdir(parents=True, exist_ok=True)
    write_header = not output_path.exists() or output_path.stat().st_size == 0
    with output_path.open("a", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=list(summary))
        if write_header:
            writer.writeheader()
        writer.writerow(summary)
    raw = {
        "command": vars(args),
        "timestamp_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "sent": sent,
        "received": received_snapshot,
        "lost": lost,
        "malformed": malformed_snapshot,
        "latency_count": len(latency_snapshot),
        "latency_min_ms": min(latency_snapshot) if latency_snapshot else None,
        "latency_max_ms": max(latency_snapshot) if latency_snapshot else None,
        "summary": summary,
    }
    Path(args.raw_output).write_text(json.dumps(raw, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(raw, indent=2))


if __name__ == "__main__":
    main()

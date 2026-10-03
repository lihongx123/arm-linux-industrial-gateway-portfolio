#!/usr/bin/env python3
"""Local vcan + MQTT: device online, stale/offline alarm, then recovery."""
import argparse
import json
import os
import pathlib
import socket
import struct
import subprocess
import threading
import time

import paho.mqtt.client as mqtt


def wait_for(predicate, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        value = predicate()
        if value:
            return value
        time.sleep(.025)
    return None


def send_can(interface, value):
    with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender:
        sender.bind((interface,))
        sender.send(struct.pack("=IB3x8s", 0x123, 1, bytes([value]) + bytes(7)))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    config = args.output / "broker.conf"
    config.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    metrics = args.output / "gateway-metrics.json"
    command = [args.binary, f"--mqtt-port={port}", f"--can-interface={args.interface}",
               "--health-stale-ms=300", "--health-recoveries=1", "--heartbeat-ms=50",
               "--diagnostics-mqtt=1", f"--metrics-file={metrics}"]
    (args.output / "run-parameters.json").write_text(json.dumps({"command": command}, indent=2))
    broker_log = (args.output / "broker.log").open("w")
    gateway_log = (args.output / "gateway.log").open("w")
    broker = gateway = None
    client = mqtt.Client(client_id=f"phase6-health-{os.getpid()}")
    subscribed = threading.Event()
    messages = []
    client.on_connect = lambda connection, _user, _flags, code: (
        connection.subscribe("gateway/mqmgateway-iot/alarm", qos=1) if code == 0 else None)
    client.on_subscribe = lambda *_: subscribed.set()
    client.on_message = lambda _connection, _user, message: messages.append(json.loads(message.payload))
    started = False
    try:
        broker = subprocess.Popen(["mosquitto", "-c", str(config)],
                                  stdout=broker_log, stderr=subprocess.STDOUT)
        def broker_ready():
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return True
            except OSError:
                return False
        if not wait_for(broker_ready, 5):
            raise RuntimeError("broker not ready")
        client.connect("127.0.0.1", port, 10)
        client.loop_start()
        started = True
        if not subscribed.wait(5):
            raise RuntimeError("alarm subscription timeout")
        gateway = subprocess.Popen(command, stdout=gateway_log, stderr=subprocess.STDOUT)
        time.sleep(.4)
        send_can(args.interface, 1)
        key = "health/can-291"
        raised = wait_for(lambda: next((event for event in messages if event.get("key") == key
                                        and event.get("action") == "raised"), None), 5)
        if raised:
            send_can(args.interface, 2)
        recovered = wait_for(lambda: next((event for event in messages if event.get("key") == key
                                           and event.get("action") == "recovered"), None), 5)
        gateway.terminate()
        gateway.wait(timeout=10)
        (args.output / "mqtt-observed.json").write_text(json.dumps(messages, indent=2))
        summary = {"result": "PASS" if raised and recovered and gateway.returncode == 0
                   and raised["sequence"] < recovered["sequence"] else "FAIL",
                   "device_id": "can-291", "raised": raised, "recovered": recovered,
                   "gateway_exit": gateway.returncode}
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary))
        return 0 if summary["result"] == "PASS" else 1
    finally:
        for process in (gateway, broker):
            if process and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        if started:
            client.loop_stop()
            client.disconnect()
        broker_log.close()
        gateway_log.close()


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Real gateway process: bounded queue stall, alarm, acknowledgement-free recovery."""
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


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def wait_for(predicate, seconds):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        value = predicate()
        if value:
            return value
        time.sleep(.025)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    port = free_port()
    config = args.output / "broker.conf"
    config.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    metrics = args.output / "gateway-metrics.json"
    command = [args.binary, f"--mqtt-port={port}", f"--can-interface={args.interface}",
               "--workers=1", "--queue-capacity=8", "--processing-delay-ms=1000",
               "--heartbeat-ms=50", "--watchdog-queue-ms=150", "--diagnostics-mqtt=1",
               f"--metrics-file={metrics}"]
    (args.output / "run-parameters.json").write_text(json.dumps({"command": command}, indent=2))
    broker_log = (args.output / "broker.log").open("w")
    gateway_log = (args.output / "gateway.log").open("w")
    broker = gateway = None
    observer = mqtt.Client(client_id=f"phase6-observer-{os.getpid()}")
    subscribed = threading.Event()
    messages = []
    observer.on_connect = lambda client, _user, _flags, code: (
        client.subscribe([("gateway/mqmgateway-iot/diagnostics/#", 1),
                          ("gateway/mqmgateway-iot/alarm", 1)]) if code == 0 else None)
    observer.on_subscribe = lambda *_: subscribed.set()
    observer.on_message = lambda _client, _user, message: messages.append(
        (message.topic, json.loads(message.payload)))
    observer_started = False
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
        observer.connect("127.0.0.1", port, 10)
        observer.loop_start()
        observer_started = True
        if not subscribed.wait(5):
            raise RuntimeError("diagnostics MQTT subscription timeout")
        gateway = subprocess.Popen(command, stdout=gateway_log, stderr=subprocess.STDOUT)
        time.sleep(.4)
        with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender:
            sender.bind((args.interface,))
            for seq in range(4):
                sender.send(struct.pack("=IB3x8s", 0x123, 8, struct.pack("!Q", seq)))

        def snapshot():
            try:
                return json.loads(metrics.read_text())
            except (OSError, json.JSONDecodeError):
                return None
        stalled = wait_for(lambda: (data if (data := snapshot()) and
                                    data["queue_watchdog"]["stalled"] else None), 4)
        if stalled:
            (args.output / "stalled-metrics.json").write_text(json.dumps(stalled, indent=2))
            observer.publish("gateway/mqmgateway-iot/diagnostics/ack",
                             json.dumps({"key": "system/queue_stalled"}), qos=1)
        acknowledged = wait_for(lambda: any(topic.endswith("/ack/result") and
                                              data.get("status") == "acknowledged"
                                              for topic, data in messages), 2)
        recovered = wait_for(lambda: (data if (data := snapshot()) and
                                      data["queue_watchdog"]["recoveries"] >= 1 and
                                      data["current_depth"] == 0 else None), 8)
        if recovered:
            (args.output / "recovered-metrics.json").write_text(json.dumps(recovered, indent=2))
        alarm_messages = wait_for(lambda: [data for topic, data in messages
                                           if topic == "gateway/mqmgateway-iot/alarm"
                                           and data.get("key") == "system/queue_stalled"]
                                  if {"raised", "acknowledged", "recovered"}.issubset({
                                      data.get("action") for topic, data in messages
                                      if topic == "gateway/mqmgateway-iot/alarm"}) else None, 3)
        gateway.terminate()
        gateway.wait(timeout=10)
        (args.output / "mqtt-observed.json").write_text(json.dumps(messages, indent=2))
        alarm_sequences = [data["sequence"] for data in alarm_messages] if alarm_messages else []
        result = {
            "result": "PASS" if stalled and recovered and acknowledged and gateway.returncode == 0 and
                      stalled["diagnostics"]["alarms"]["active"] >= 1 and
                      recovered["diagnostics"]["alarms"]["active"] == 0 and
                      recovered["diagnostics"]["alarms"]["events_total"] >= 3 and
                      any(topic == "gateway/mqmgateway-iot/diagnostics" for topic, _ in messages) and
                      bool(alarm_messages) and alarm_sequences == sorted(set(alarm_sequences))
                      else "FAIL",
            "gateway_exit": gateway.returncode,
            "stalled_observed": bool(stalled), "recovered_observed": bool(recovered),
            "mqtt_acknowledged": bool(acknowledged), "mqtt_diagnostics_messages": sum(
                topic == "gateway/mqmgateway-iot/diagnostics" for topic, _ in messages),
            "alarm_events": recovered["diagnostics"]["alarms"]["events_total"] if recovered else None,
            "mqtt_alarm_actions": sorted({data.get("action") for data in alarm_messages}) if alarm_messages else [],
            "mqtt_alarm_sequences": alarm_sequences,
            "stall_events": recovered["queue_watchdog"]["stall_events"] if recovered else None,
            "recoveries": recovered["queue_watchdog"]["recoveries"] if recovered else None,
        }
        (args.output / "summary.json").write_text(json.dumps(result, indent=2))
        print(json.dumps(result))
        return 0 if result["result"] == "PASS" else 1
    finally:
        for process in (gateway, broker):
            if process and process.poll() is None:
                process.terminate()
                process.wait(timeout=10)
        if observer_started:
            observer.loop_stop()
            observer.disconnect()
        broker_log.close()
        gateway_log.close()


if __name__ == "__main__":
    raise SystemExit(main())

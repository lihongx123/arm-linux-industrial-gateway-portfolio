#!/usr/bin/env python3
"""Local Mosquitto + generic TCP device: real command-id/result lifecycle."""
import argparse
import json
import pathlib
import subprocess
import threading
import time

import paho.mqtt.client as mqtt
from tcp_mqtt_integration_test import DeviceServer, free_port, wait_for


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    port = free_port()
    config = args.output / "broker.conf"
    config.write_text(f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    gateway_log = (args.output / "gateway.log").open("w")
    broker_log = (args.output / "broker.log").open("w")
    device = DeviceServer("generic")
    received = []
    mutex = threading.Lock()
    observer = mqtt.Client(client_id="northbound-lifecycle-observer", clean_session=True)
    observer.on_connect = lambda client, _user, _flags, code: client.subscribe("device/#", 1) if code == 0 else None

    def on_message(_client, _user, message):
        try:
            body = json.loads(message.payload)
        except json.JSONDecodeError:
            return
        with mutex:
            received.append({"topic": message.topic, "body": body})

    observer.on_message = on_message
    broker = gateway = None
    try:
        device.start()
        broker = subprocess.Popen(["mosquitto", "-c", str(config)], stdout=broker_log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                observer.connect("127.0.0.1", port, 5)
                break
            except OSError:
                time.sleep(.05)
        else:
            raise RuntimeError("broker startup timeout")
        observer.loop_start()
        gateway = subprocess.Popen([args.binary, f"--mqtt-port={port}", "--can-interface=vcan0",
                                    f"--generic-tcp=sensor,127.0.0.1,{device.port}",
                                    f"--metrics-file={args.output / 'metrics.json'}"],
                                   stdout=gateway_log, stderr=subprocess.STDOUT)

        def status(command_id, state):
            with mutex:
                return next((item for item in received if item["topic"] == "device/sensor/status" and
                             item["body"].get("command_id") == command_id and
                             item["body"].get("state") == state), None)

        if not wait_for(lambda: any(item["topic"] == "device/sensor/telemetry" for item in received), 5):
            raise RuntimeError("no upstream telemetry")
        topic = "device/sensor/cmd/tcp_send"
        observer.publish(topic, json.dumps({"command_id": "nb-success", "data": "0405"}), qos=1)
        if not wait_for(lambda: status("nb-success", "succeeded"), 5):
            raise RuntimeError("correlated success result absent")
        if not status("nb-success", "accepted"):
            raise RuntimeError("admission acknowledgement absent")
        if device.commands.count("0405") != 1:
            raise RuntimeError("command not executed once")

        observer.publish(topic, json.dumps({"command_id": "nb-success", "data": "0405"}), qos=1)
        if not wait_for(lambda: status("nb-success", "rejected"), 5):
            raise RuntimeError("duplicate command not rejected")
        if status("nb-success", "rejected")["body"].get("detail") != "duplicate command_id":
            raise RuntimeError("duplicate rejection reason missing")
        observer.publish(topic, json.dumps({"command_id": "nb-expired", "data": "0607", "deadline_ms": 1}), qos=1)
        if not wait_for(lambda: status("nb-expired", "timeout"), 5):
            raise RuntimeError("expired command not timed out")
        observer.publish(topic, json.dumps({"command_id": "nb-invalid", "data": ""}), qos=1)
        if not wait_for(lambda: status("nb-invalid", "rejected"), 5):
            raise RuntimeError("invalid command not rejected")
        if device.commands != ["0405"]:
            raise RuntimeError("duplicate/expired/invalid command reached device")
        if gateway.poll() is not None or device.error:
            raise RuntimeError("gateway or device process failed")
        with mutex:
            (args.output / "mqtt-observed.json").write_text(json.dumps(received, indent=2))
        def updated_metrics():
            try:
                snapshot = json.loads((args.output / "metrics.json").read_text())
                northbound = snapshot["northbound"]
                return snapshot if northbound["duplicate_commands"] == 1 and northbound["timed_out_commands"] >= 1 else None
            except (FileNotFoundError, json.JSONDecodeError, KeyError):
                return None

        if not wait_for(updated_metrics, 5):
            raise RuntimeError("northbound lifecycle metrics missing")
        metrics = updated_metrics()
        result = {"result": "PASS", "broker": "local Mosquitto", "device": "generic TCP simulator",
                  "executed_commands": device.commands, "success_command_id": "nb-success",
                  "duplicate_rejected": True, "expired_timed_out": True, "invalid_rejected": True,
                  "northbound_metrics": metrics["northbound"]}
        (args.output / "summary.json").write_text(json.dumps(result, indent=2))
        print(json.dumps(result))
    finally:
        if gateway and gateway.poll() is None:
            gateway.terminate()
            gateway.wait(timeout=5)
        observer.loop_stop()
        observer.disconnect()
        if broker and broker.poll() is None:
            broker.terminate()
            broker.wait(timeout=5)
        device.close()
        broker_log.close()
        gateway_log.close()


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Local Mosquitto/vcan burst: prove command path progresses through telemetry congestion."""
import argparse
import json
import pathlib
import socket
import struct
import subprocess
import threading
import time

import paho.mqtt.client as mqtt


def wait_for(fn, seconds):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        result = fn()
        if result:
            return result
        time.sleep(.025)
    return None


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--legacy-comparison", action="store_true")
    parser.add_argument("--new-comparison", action="store_true")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    with socket.socket() as probe:
        probe.bind(("127.0.0.1", 0))
        port = probe.getsockname()[1]
    (args.output / "broker.conf").write_text(
        f"listener {port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    metrics_path = args.output / "metrics.json"
    command = [args.binary, f"--mqtt-port={port}", "--can-interface=vcan0",
               "--workers=1", "--processing-delay-ms=200", "--heartbeat-ms=50",
               f"--metrics-file={metrics_path}"]
    if args.legacy_comparison:
        command.append("--queue-capacity=4")
    else:
        command += ["--telemetry-queue-capacity=4", "--command-queue-capacity=4",
                    "--command-workers=1",
                    f"--mqtt-outbound-capacity={4 if args.new_comparison else 64}"]
        if not args.new_comparison:
            command.append("--point-policy=can-291,frame,1,0,0")
    (args.output / "parameters.json").write_text(json.dumps({"command": command}, indent=2))
    broker_log = (args.output / "broker.log").open("w")
    gateway_log = (args.output / "gateway.log").open("w")
    broker = gateway = None
    observer = mqtt.Client(client_id="phase65-isolation")
    received = []
    lock = threading.Lock()
    subscribed = threading.Event()
    observer.on_connect = lambda client, _user, _flags, code: (
        client.subscribe("device/#", 1) if code == 0 else None)
    observer.on_subscribe = lambda *_: subscribed.set()
    def on_message(_client, _user, message):
        try:
            body = json.loads(message.payload)
        except json.JSONDecodeError:
            return
        with lock:
            received.append({"topic": message.topic, "body": body, "at": time.monotonic()})
    observer.on_message = on_message
    try:
        broker = subprocess.Popen(["mosquitto", "-c", str(args.output / "broker.conf")],
                                  stdout=broker_log, stderr=subprocess.STDOUT)
        def broker_ready():
            try:
                with socket.create_connection(("127.0.0.1", port), timeout=.1):
                    return True
            except OSError:
                return False
        if not wait_for(broker_ready, 5):
            raise RuntimeError("broker did not start")
        observer.connect("127.0.0.1", port, 10)
        observer.loop_start()
        if not subscribed.wait(5):
            raise RuntimeError("MQTT observer did not subscribe")
        gateway = subprocess.Popen(command, stdout=gateway_log, stderr=subprocess.STDOUT)
        time.sleep(.4)
        with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender:
            sender.bind(("vcan0",))
            for n in range(200):
                sender.send(struct.pack("=IB3x8s", 0x123, 8, struct.pack("!Q", n)))
        def metrics():
            try:
                return json.loads(metrics_path.read_text())
            except (OSError, json.JSONDecodeError):
                return None
        congested = wait_for(lambda: (m if (m := metrics()) and
                              m["rejected" if args.legacy_comparison else "telemetry_queue_rejected"] > 0 else None), 5)
        if not congested:
            raise RuntimeError("telemetry queue did not saturate")
        sent_at = time.monotonic()
        observer.publish("device/can-291/cmd/can_tx",
                         json.dumps({"command_id": "phase65-control", "can_id": 291,
                                     "data": "0102", "timeout_ms": 1500}), qos=1)
        def result(state):
            with lock:
                return next((m for m in received if
                             m["topic"] == "device/can-291/status" and
                             m["body"].get("command_id") == "phase65-control" and
                             m["body"].get("state") == state), None)
        if args.legacy_comparison or args.new_comparison:
            first = wait_for(lambda: result("accepted") or result("rejected"), 3)
            terminal = wait_for(lambda: result("succeeded") or result("rejected") or result("timeout"), 3)
            summary = {
                "mode": "legacy-shared" if args.legacy_comparison else "phase65-split",
                "telemetry_queue_capacity": 4, "telemetry_workers": 1,
                "processing_delay_ms": 200, "flood_frames": 200,
                "telemetry_rejected": congested["rejected" if args.legacy_comparison else "telemetry_queue_rejected"],
                "command_first_state": first["body"].get("state") if first else "no_response",
                "command_first_ms": round((first["at"] - sent_at) * 1000, 3) if first else None,
                "command_terminal_state": terminal["body"].get("state") if terminal else "no_response",
                "command_terminal_ms": round((terminal["at"] - sent_at) * 1000, 3) if terminal else None}
            with lock:
                (args.output / "mqtt-observed.json").write_text(json.dumps(received, indent=2))
            (args.output / "summary.json").write_text(json.dumps(summary, indent=2))
            print(json.dumps(summary))
            return
        accepted = wait_for(lambda: result("accepted"), 3)
        terminal = wait_for(lambda: result("succeeded"), 3)
        if not accepted or not terminal:
            raise RuntimeError("command admission or terminal result missing under telemetry pressure")
        if gateway.poll() is not None:
            raise RuntimeError("gateway exited")
        if not wait_for(lambda: (m if (m := metrics()) and
                        m["telemetry_queue"]["current_depth"] == 0 else None), 5):
            raise RuntimeError("telemetry queue did not drain")
        with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender:
            sender.bind(("vcan0",))
            identical = struct.pack("=IB3x8s", 0x123, 8, struct.pack("!Q", 10000))
            for _ in range(5):
                sender.send(identical)
        cov = wait_for(lambda: (m if (m := metrics()) and
                       m["telemetry_suppressed_cov"] >= 4 else None), 5)
        if not cov:
            raise RuntimeError("mapped repeated samples were not suppressed")
        before_inverse = cov["telemetry_queue"]["dequeued"]
        for n in range(4):
            observer.publish("device/can-291/cmd/can_tx",
                             json.dumps({"command_id": f"phase65-inverse-{n}", "can_id": 291,
                                         "data": "0304", "timeout_ms": 2500}), qos=1)
        with socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW) as sender:
            sender.bind(("vcan0",))
            sender.send(struct.pack("=IB3x8s", 0x123, 8, struct.pack("!Q", 10001)))
        progress = wait_for(lambda: (m if (m := metrics()) and
                            m["telemetry_queue"]["dequeued"] > before_inverse else None), 2)
        if not progress:
            raise RuntimeError("telemetry worker stopped progressing during command burst")
        burst_ids = [f"phase65-burst-{n}" for n in range(12)]
        for command_id in burst_ids:
            observer.publish("device/can-291/cmd/can_tx",
                             json.dumps({"command_id": command_id, "can_id": 291,
                                         "data": "0506", "timeout_ms": 5000}), qos=1)
        rejected_commands = wait_for(lambda: (m if (m := metrics()) and
                                     m["command_queue_rejected"] > 0 else None), 5)
        if not rejected_commands:
            raise RuntimeError("command queue capacity was not exercised")
        def explicit_rejection():
            with lock:
                return any(m["body"].get("command_id") in burst_ids and
                           m["body"].get("state") == "rejected" and
                           "command queue capacity exceeded" in m["body"].get("detail", "")
                           for m in received)
        if not wait_for(explicit_rejection, 5):
            raise RuntimeError("full command queue did not produce an explicit rejection")
        def terminal_ids():
            with lock:
                return {m["body"].get("command_id") for m in received
                        if m["topic"] == "device/can-291/status" and
                        m["body"].get("state") in {"succeeded", "failed", "timeout", "rejected"}}
        if not wait_for(lambda: set(burst_ids).issubset(terminal_ids()), 5):
            raise RuntimeError("not all burst commands received explicit terminal outcomes")
        final = metrics()
        summary = {"result": "PASS", "telemetry_queue_rejected": final["telemetry_queue_rejected"],
                   "command_queue_rejected": final["command_queue_rejected"],
                   "command_admission_ms": round((accepted["at"] - sent_at) * 1000, 3),
                   "command_terminal_ms": round((terminal["at"] - sent_at) * 1000, 3),
                   "telemetry_peak": final["telemetry_queue"]["peak_depth"],
                   "command_peak": final["command_queue"]["peak_depth"],
                   "cov_suppressed": final["telemetry_suppressed_cov"],
                   "telemetry_progressed_during_command_burst": True,
                   "explicit_command_queue_rejection": True,
                   "burst_explicit_terminal_outcomes": len(burst_ids)}
        with lock:
            (args.output / "mqtt-observed.json").write_text(json.dumps(received, indent=2))
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary))
    finally:
        if gateway and gateway.poll() is None:
            gateway.terminate()
            gateway.wait(timeout=5)
        observer.loop_stop()
        observer.disconnect()
        if broker and broker.poll() is None:
            broker.terminate()
            broker.wait(timeout=5)
        broker_log.close()
        gateway_log.close()


if __name__ == "__main__":
    main()

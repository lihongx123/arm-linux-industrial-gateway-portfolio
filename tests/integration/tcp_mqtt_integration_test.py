#!/usr/bin/env python3
"""Local MQTT -> GatewayCore -> two TCP devices -> MQTT integration probe."""
import argparse
import json
import pathlib
import socket
import struct
import subprocess
import tempfile
import threading
import time

import paho.mqtt.client as mqtt


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def recv_exact(sock, count):
    data = b""
    while len(data) < count:
        chunk = sock.recv(count - len(data))
        if not chunk:
            raise ConnectionError("peer closed")
        data += chunk
    return data


class DeviceServer:
    def __init__(self, kind):
        self.kind = kind
        self.port = free_port()
        self.stop = threading.Event()
        self.commands = []
        self.error = None
        self.thread = threading.Thread(target=self.run, daemon=True)

    def run(self):
        try:
            with socket.socket() as listener:
                listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
                listener.bind(("127.0.0.1", self.port))
                listener.listen()
                listener.settimeout(.2)
                while not self.stop.is_set():
                    try:
                        peer, _ = listener.accept()
                    except socket.timeout:
                        continue
                    with peer:
                        peer.settimeout(.2)
                        if self.kind == "generic":
                            peer.sendall(b"\x00\x02\xaa\xbb")
                        while not self.stop.is_set():
                            try:
                                if self.kind == "modbus":
                                    mbap = recv_exact(peer, 7)
                                    body = recv_exact(peer, (mbap[4] << 8 | mbap[5]) - 1)
                                    if body[0] == 3:
                                        response = mbap[:4] + b"\x00\x05" + mbap[6:] + b"\x03\x02\x00\x2a"
                                        peer.sendall(response)
                                    elif body[0] == 6:
                                        self.commands.append(body.hex())
                                        peer.sendall(mbap + body)
                                else:
                                    length = int.from_bytes(recv_exact(peer, 2), "big")
                                    self.commands.append(recv_exact(peer, length).hex())
                            except socket.timeout:
                                continue
                            except ConnectionError:
                                break
        except Exception as error:
            self.error = str(error)

    def start(self):
        self.thread.start()

    def close(self):
        self.stop.set()
        self.thread.join(timeout=2)


def wait_for(predicate, seconds=8):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(.02)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--telemetry-qos", type=int, choices=(0, 1), default=1)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    broker_port = free_port()
    config = args.output / "broker.conf"
    config.write_text(f"listener {broker_port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    gateway_log = (args.output / "gateway.log").open("w")
    broker_log = (args.output / "broker.log").open("w")
    modbus = DeviceServer("modbus")
    generic = DeviceServer("generic")
    messages = []
    lock = threading.Lock()
    observer = mqtt.Client(client_id="tcp-phase3-observer", clean_session=True)
    observer.on_connect = lambda c, u, f, rc: c.subscribe("device/#", 1) if rc == 0 else None

    def received(client, userdata, message):
        with lock:
            messages.append((message.topic, message.payload.decode()))

    observer.on_message = received
    broker = gateway = None
    try:
        modbus.start()
        generic.start()
        broker = subprocess.Popen(["mosquitto", "-c", str(config)], stdout=broker_log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                observer.connect("127.0.0.1", broker_port, 5)
                break
            except OSError:
                time.sleep(.05)
        else:
            raise RuntimeError("broker startup")
        observer.loop_start()
        binary = pathlib.Path(args.binary).resolve()
        gateway = subprocess.Popen([
            str(binary), f"--mqtt-port={broker_port}", "--can-interface=vcan0",
            f"--telemetry-qos={args.telemetry_qos}",
            f"--modbus-tcp=plc,127.0.0.1,{modbus.port},1,0,100,500,100",
            f"--generic-tcp=sensor,127.0.0.1,{generic.port}",
            f"--metrics-file={args.output / 'metrics.json'}",
        ], stdout=gateway_log, stderr=subprocess.STDOUT)
        def find(topic, expected=None):
            with lock:
                return any(t == topic and (expected is None or expected in payload) for t, payload in messages)
        if not wait_for(lambda: find("device/plc/telemetry") and find("device/sensor/telemetry")):
            raise RuntimeError("TCP to MQTT telemetry missing")
        if not (find("device/plc/telemetry", '"point_id":"holding-0"') and
                find("device/plc/telemetry", '"value":"42"') and
                find("device/sensor/telemetry", '"point_id":"payload"') and
                find("device/sensor/telemetry", '"raw_value":"aabb"')):
            raise RuntimeError("point registry/mapper missing from production telemetry")
        observer.publish("device/plc/cmd/modbus_tcp_write",
                         json.dumps({"slave": 1, "register": 7, "value": 1234}), qos=1)
        observer.publish("device/sensor/cmd/tcp_send", json.dumps({"data": "0405"}), qos=1)
        if not wait_for(lambda: find("device/plc/status", '"status":"ok"') and
                        find("device/sensor/status", '"status":"ok"') and
                        "0405" in generic.commands and bool(modbus.commands)):
            raise RuntimeError("MQTT to TCP command/status missing")
        observer.publish("device/sensor/cmd/tcp_send", json.dumps({"data": ""}), qos=1)
        if not wait_for(lambda: find("device/sensor/status", '"status":"rejected"')):
            raise RuntimeError("invalid TCP command not rejected")
        if gateway.poll() is not None or modbus.error or generic.error:
            raise RuntimeError("gateway/device server exited")
        summary = {"result": "PASS", "protocols": ["modbus_tcp", "generic_tcp"],
                   "plc_writes": modbus.commands, "generic_writes": generic.commands,
                   "telemetry_seen": ["plc", "sensor"], "command_status": "ok",
                   "invalid_command": "rejected"}
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary))
    finally:
        if gateway:
            gateway.terminate()
            try:
                gateway.wait(timeout=4)
            except subprocess.TimeoutExpired:
                gateway.kill()
                gateway.wait()
        observer.loop_stop()
        observer.disconnect()
        if broker:
            broker.terminate()
            broker.wait(timeout=4)
        modbus.close()
        generic.close()
        gateway_log.close()
        broker_log.close()


if __name__ == "__main__":
    main()

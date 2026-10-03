#!/usr/bin/env python3
"""Local MQTT -> GatewayCore -> real MC 3E binary/TCP test server -> MQTT."""
import argparse
import json
import pathlib
import socket
import subprocess
import threading
import time

import paho.mqtt.client as mqtt


def free_port():
    with socket.socket() as sock:
        sock.bind(("127.0.0.1", 0))
        return sock.getsockname()[1]


def exact(sock, length):
    data = b""
    while len(data) < length:
        part = sock.recv(length - len(data))
        if not part:
            raise ConnectionError("peer closed")
        data += part
    return data


class McServer:
    def __init__(self):
        self.port = free_port()
        self.stop = threading.Event()
        self.writes = []
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
                        while not self.stop.is_set():
                            try:
                                header = exact(peer, 9)
                                if header[:7] != b"\x50\x00\x00\xff\xff\x03\x00":
                                    raise ValueError("not MC 3E binary CPU route")
                                body = exact(peer, int.from_bytes(header[7:9], "little"))
                                if body[:2] != b"\x10\x00" or body[4:6] != b"\x00\x00" or \
                                        body[6:9] != b"\x07\x00\x00" or body[9:12] != b"\xa8\x01\x00":
                                    raise ValueError("invalid MC request body")
                                response = b"\xd0\x00\x00\xff\xff\x03\x00"
                                if body[2:4] == b"\x01\x04" and len(body) == 12:
                                    peer.sendall(response + b"\x04\x00\x00\x00\x34\x12")
                                elif body[2:4] == b"\x01\x14" and len(body) == 14:
                                    self.writes.append(body[12:14].hex())
                                    peer.sendall(response + b"\x02\x00\x00\x00")
                                else:
                                    raise ValueError("unsupported MC command")
                            except socket.timeout:
                                continue
                            except ConnectionError:
                                break
        except Exception as error:
            self.error = str(error)


def wait_for(predicate, seconds=8):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return True
        time.sleep(.02)
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--binary", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    broker_port = free_port()
    (args.output / "broker.conf").write_text(
        f"listener {broker_port} 127.0.0.1\nallow_anonymous true\npersistence false\n")
    broker_log = (args.output / "broker.log").open("w")
    gateway_log = (args.output / "gateway.log").open("w")
    server = McServer()
    messages = []
    lock = threading.Lock()
    observer = mqtt.Client(client_id="mc-phase5-observer", clean_session=True)
    observer.on_connect = lambda client, data, flags, code: client.subscribe("device/#", 1) if code == 0 else None
    def received(client, data, message):
        with lock:
            messages.append((message.topic, message.payload.decode()))
    observer.on_message = received
    broker = gateway = None
    try:
        server.thread.start()
        broker = subprocess.Popen(["mosquitto", "-c", str(args.output / "broker.conf")],
                                  stdout=broker_log, stderr=subprocess.STDOUT)
        for _ in range(100):
            try:
                observer.connect("127.0.0.1", broker_port, 5)
                break
            except OSError:
                time.sleep(.05)
        else:
            raise RuntimeError("broker startup")
        observer.loop_start()
        gateway = subprocess.Popen([
            str(pathlib.Path(args.binary).resolve()), f"--mqtt-port={broker_port}",
            "--can-interface=vcan0", f"--mc=mc-prod,speed,127.0.0.1,{server.port},7,60,500,50",
            f"--metrics-file={args.output / 'metrics.json'}",
        ], stdout=gateway_log, stderr=subprocess.STDOUT)

        def seen(topic, text):
            with lock:
                return any(t == topic and text in payload for t, payload in messages)

        if not wait_for(lambda: seen("device/mc-prod/telemetry", '"point_id":"speed"') and
                        seen("device/mc-prod/telemetry", '"value":"4660"')):
            raise RuntimeError("mapped MC telemetry missing")
        observer.publish("device/mc-prod/cmd/mc_write", json.dumps({"register": 7, "value": 4660}), qos=1)
        if not wait_for(lambda: seen("device/mc-prod/status", '"status":"accepted"') and
                        seen("device/mc-prod/status", '"status":"success"') and "3412" in server.writes):
            raise RuntimeError("MC accepted/acknowledged write missing")
        observer.publish("device/mc-prod/cmd/mc_write", json.dumps({"register": 8, "value": 1}), qos=1)
        if not wait_for(lambda: seen("device/mc-prod/status", '"status":"rejected"')):
            raise RuntimeError("wrong D address not rejected")
        if gateway.poll() is not None or server.error:
            raise RuntimeError("gateway or MC server failed: " + str(server.error))
        summary = {"result": "PASS", "protocol": "MC 3E binary TCP D-word",
                   "point_id": "speed", "mapped_value": "4660", "wire_write_le": server.writes,
                   "command_status": ["accepted", "success"], "invalid_address": "rejected"}
        (args.output / "summary.json").write_text(json.dumps(summary, indent=2))
        print(json.dumps(summary))
    finally:
        if gateway:
            gateway.terminate()
            try:
                gateway.wait(timeout=4)
            except subprocess.TimeoutExpired:
                gateway.kill(); gateway.wait()
        observer.loop_stop(); observer.disconnect()
        if broker:
            broker.terminate(); broker.wait(timeout=4)
        server.stop.set(); server.thread.join(timeout=2)
        broker_log.close(); gateway_log.close()


if __name__ == "__main__":
    main()

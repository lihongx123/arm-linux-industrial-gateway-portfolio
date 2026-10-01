#!/usr/bin/env python3
"""Paced SocketCAN -> gateway -> EMQX soak with independent MQTT verification."""

import argparse
import collections
import datetime as dt
import json
import os
import secrets
import signal
import socket
import struct
import subprocess
import threading
import time
from pathlib import Path

import paho.mqtt.client as mqtt


def utc_now():
    return dt.datetime.now(dt.timezone.utc).isoformat(timespec="milliseconds")


def save_text(path, text):
    path.write_text(text, encoding="utf-8")


def save_json(path, value):
    save_text(path, json.dumps(value, indent=2, ensure_ascii=False) + "\n")


def require_environment():
    names = ["EMQX_HOST", "EMQX_PORT", "EMQX_CA", "GATEWAY_MQTT_USERNAME", "GATEWAY_MQTT_PASSWORD"]
    missing = [name for name in names if not os.environ.get(name)]
    if missing:
        raise RuntimeError("missing environment variables: " + ", ".join(missing))
    if os.environ["EMQX_HOST"] != "kf7bd96b.ala.dedicated.aliyun.emqxcloud.cn":
        raise RuntimeError("unexpected EMQX_HOST")
    if os.environ["EMQX_PORT"] != "8883":
        raise RuntimeError("unexpected EMQX_PORT")
    if os.environ["GATEWAY_MQTT_USERNAME"] != "gateway-client":
        raise RuntimeError("unexpected MQTT username")
    if not Path(os.environ["EMQX_CA"]).is_file():
        raise RuntimeError("CA file is unavailable")


def mqtt_client(client_id, on_connect, on_message=None, on_subscribe=None):
    client = mqtt.Client(client_id=client_id, clean_session=True, protocol=mqtt.MQTTv311)
    client.username_pw_set(os.environ["GATEWAY_MQTT_USERNAME"], os.environ["GATEWAY_MQTT_PASSWORD"])
    client.tls_set(ca_certs=os.environ["EMQX_CA"])
    client.tls_insecure_set(False)
    client.on_connect = on_connect
    if on_message:
        client.on_message = on_message
    if on_subscribe:
        client.on_subscribe = on_subscribe
    client.connect(os.environ["EMQX_HOST"], int(os.environ["EMQX_PORT"]), keepalive=30)
    client.loop_start()
    return client


def can_socket(can_id=None):
    sock = socket.socket(socket.PF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    if can_id is not None:
        sock.setsockopt(socket.SOL_CAN_RAW, socket.CAN_RAW_FILTER, struct.pack("=II", can_id, 0x1FFFFFFF))
    sock.bind(("vcan0",))
    return sock


def can_frame(can_id, payload):
    return struct.pack("=IB3x8s", can_id, len(payload), payload.ljust(8, b"\0"))


def parse_args():
    parser = argparse.ArgumentParser()
    parser.add_argument("--rate", type=int, default=100)
    parser.add_argument("--duration", type=int, default=1800)
    parser.add_argument("--commands", type=int, default=30)
    parser.add_argument("--results", type=Path, required=True)
    parser.add_argument("--gateway", type=Path, default=Path("/tmp/mqmgateway-cloud-live-native/src/iot_gateway/mqmgateway_iot"))
    return parser.parse_args()


def main():
    args = parse_args()
    require_environment()
    if args.rate <= 0 or args.duration <= 0 or args.commands < 0:
        raise RuntimeError("rate and duration must be positive; commands must be nonnegative")
    if args.results.exists() and any(args.results.iterdir()):
        raise RuntimeError("refusing to overwrite nonempty evidence directory")
    args.results.mkdir(parents=True, exist_ok=True)
    if not args.gateway.is_file():
        raise RuntimeError("gateway binary is unavailable")

    total_target = args.rate * args.duration
    nonce = secrets.token_bytes(4)
    run_id = nonce.hex()
    telemetry_id = 0x321
    command_can_id = 0x456
    device_id = f"can-{telemetry_id}"
    telemetry_topic = f"resume/gateway/devices/{device_id}/telemetry"
    command_topic = "resume/gateway/devices/cloud-soak-001/command"
    result_topic = "resume/gateway/devices/cloud-soak-001/command/result"
    status_topic = f"resume/gateway/status/cloud-soak-{run_id}"
    save_text(args.results / "environment.txt", "\n".join([
        f"broker={os.environ['EMQX_HOST']}:{os.environ['EMQX_PORT']}",
        f"username={os.environ['GATEWAY_MQTT_USERNAME']}",
        f"ca={os.environ['EMQX_CA']}",
        "tls_peer_verification=enabled", "tls_hostname_verification=enabled",
        "qos=1", "can_interface=vcan0", f"telemetry_topic={telemetry_topic}",
        f"command_topic={command_topic}", f"result_topic={result_topic}",
        f"run_id={run_id}", f"target_rate={args.rate}", f"target_duration_seconds={args.duration}",
        f"target_messages={total_target}", "credentials=process environment only", "",
    ]))

    lock = threading.Lock()
    subscriber_ready = threading.Event()
    publisher_ready = threading.Event()
    gateway_online = threading.Event()
    seen = bytearray(total_target + 1)
    samples_first = []
    samples_last = collections.deque(maxlen=20)
    duplicates = []
    anomalies = []
    received_total = 0
    received_unique = 0
    out_of_order = 0
    max_seq = 0
    result_count = 0
    result_failures = 0
    routed = set()
    result_samples = []
    connection_count = 0
    subscriber_connections = 0
    generated = 0
    commands_sent = 0
    gateway = None
    sender = None
    monitor = None
    subscriber = None
    publisher = None
    monitor_stop = threading.Event()
    monitor_thread = None
    gateway_log = None
    start_wall = None
    end_wall = None
    start_mono = None
    end_mono = None
    run_error = None
    sustained_lag_windows = 0

    def on_subscriber_connect(client, _userdata, _flags, rc):
        nonlocal subscriber_connections
        if rc != 0:
            return
        with lock:
            subscriber_connections += 1
        client.subscribe([(telemetry_topic, 1), (result_topic, 1), (status_topic, 1)])

    def on_subscribe(_client, _userdata, _mid, granted_qos):
        if all(qos == 1 for qos in granted_qos):
            subscriber_ready.set()

    def on_message(_client, _userdata, message):
        nonlocal received_total, received_unique, out_of_order, max_seq
        nonlocal result_count, result_failures
        if message.topic == status_topic:
            try:
                if json.loads(message.payload).get("status") == "online":
                    gateway_online.set()
            except (ValueError, UnicodeError):
                pass
            return
        if message.topic == result_topic:
            try:
                outcome = json.loads(message.payload)
            except (ValueError, UnicodeError):
                outcome = {"status": "invalid", "detail": "malformed result"}
            with lock:
                result_count += 1
                if outcome.get("status") != "ok":
                    result_failures += 1
                if len(result_samples) < 40:
                    result_samples.append({"received_at": utc_now(), "result": outcome})
            return
        if message.topic != telemetry_topic:
            return
        try:
            item = json.loads(message.payload)
            raw = bytes.fromhex(item["payload"])
            if item.get("device_id") != device_id or item.get("address") != telemetry_id or len(raw) != 8:
                raise ValueError("device, address, or frame length mismatch")
            if raw[:4] != nonce:
                return  # Ignore stale retained or other run traffic.
            seq = int.from_bytes(raw[4:], "big")
            if not 1 <= seq <= total_target:
                raise ValueError("sequence outside expected range")
        except (ValueError, KeyError, TypeError, UnicodeError) as exc:
            with lock:
                if len(anomalies) < 100:
                    anomalies.append(str(exc))
            return
        with lock:
            received_total += 1
            if seen[seq]:
                duplicates.append(seq)
            else:
                seen[seq] = 1
                received_unique += 1
                if seq < max_seq:
                    out_of_order += 1
                max_seq = max(max_seq, seq)
            sample = {"received_at": utc_now(), "seq": seq, "device_id": device_id, "payload_hex": raw.hex()}
            if len(samples_first) < 20:
                samples_first.append(sample)
            samples_last.append(sample)

    def on_publisher_connect(_client, _userdata, _flags, rc):
        if rc == 0:
            publisher_ready.set()

    def monitor_can():
        while not monitor_stop.is_set():
            try:
                data = monitor.recv(16)
            except socket.timeout:
                continue
            except OSError:
                break
            if len(data) != 16:
                continue
            can_id, length = struct.unpack_from("=IB", data)
            if can_id & 0x1FFFFFFF != command_can_id or length != 4:
                continue
            seq = int.from_bytes(data[8:12], "big")
            with lock:
                if 1 <= seq <= args.commands:
                    routed.add(seq)

    try:
        subscriber = mqtt_client(f"cloud-soak-sub-{run_id}", on_subscriber_connect, on_message, on_subscribe)
        if not subscriber_ready.wait(20):
            raise RuntimeError("independent TLS subscriber did not subscribe at QoS 1")
        publisher = mqtt_client(f"cloud-soak-cmd-{run_id}", on_publisher_connect)
        if not publisher_ready.wait(20):
            raise RuntimeError("independent TLS command publisher did not connect")
        sender = can_socket()
        monitor = can_socket(command_can_id)
        monitor.settimeout(0.5)
        monitor_thread = threading.Thread(target=monitor_can, daemon=True)
        monitor_thread.start()
        gateway_log = (args.results / "gateway.log").open("w", encoding="utf-8")
        gateway = subprocess.Popen([
            str(args.gateway), "--cloud", f"--client-id=cloud-soak-{run_id}",
            "--can-interface=vcan0", "--heartbeat-ms=1000", "--pipeline-metrics=1",
            f"--metrics-file={args.results / 'gateway-metrics.json'}",
        ], stdout=gateway_log, stderr=subprocess.STDOUT, env=os.environ.copy())
        if not gateway_online.wait(30):
            raise RuntimeError("gateway did not authenticate and publish online status")

        start_wall = utc_now()
        start_mono = time.monotonic()
        next_command = 0
        next_progress = start_mono + 30
        for seq in range(1, total_target + 1):
            due = start_mono + (seq - 1) / args.rate
            delay = due - time.monotonic()
            if delay > 0:
                time.sleep(delay)
            sender.send(can_frame(telemetry_id, nonce + seq.to_bytes(4, "big")))
            generated = seq
            elapsed = time.monotonic() - start_mono
            if next_command < args.commands and elapsed >= next_command * args.duration / args.commands:
                command_seq = next_command + 1
                command_id = f"{run_id}-{command_seq:03d}"
                payload = json.dumps({
                    "command_id": command_id,
                    "can_id": command_can_id,
                    "data": command_seq.to_bytes(4, "big").hex(),
                    "timeout_ms": 5000,
                }, separators=(",", ":"))
                message = publisher.publish(command_topic, payload, qos=1)
                if message.rc != mqtt.MQTT_ERR_SUCCESS:
                    raise RuntimeError(f"command publish rejected at seq {command_seq}: rc={message.rc}")
                message.wait_for_publish(timeout=10)
                if not message.is_published():
                    raise RuntimeError(f"command PUBACK timeout at seq {command_seq}")
                commands_sent += 1
                next_command += 1
            if gateway.poll() is not None:
                raise RuntimeError(f"gateway exited early: code {gateway.returncode}")
            if time.monotonic() >= next_progress:
                with lock:
                    progress = {"at": utc_now(), "elapsed_s": round(elapsed, 3), "generated": generated,
                                "cloud_received_total": received_total, "cloud_received_unique": received_unique,
                                "commands_sent": commands_sent, "commands_routed": len(routed),
                                "command_results_received": result_count}
                save_json(args.results / "progress.json", progress)
                if args.rate > 100:
                    try:
                        snapshot = json.loads((args.results / "gateway-metrics.json").read_text(encoding="utf-8"))
                    except (OSError, ValueError):
                        snapshot = {}
                    if snapshot.get("publish_failures", 0) or snapshot.get("rejected", 0):
                        raise RuntimeError("optional higher-rate run stopped after gateway failures")
                    lag = generated - progress["cloud_received_unique"]
                    sustained_lag_windows = sustained_lag_windows + 1 if lag > args.rate * 10 else 0
                    if sustained_lag_windows >= 3:
                        raise RuntimeError("optional higher-rate run stopped after sustained cloud receive lag")
                next_progress += 30
        finish_due = start_mono + args.duration
        if finish_due > time.monotonic():
            time.sleep(finish_due - time.monotonic())
        end_mono = time.monotonic()
        end_wall = utc_now()
        deadline = time.monotonic() + 90
        while time.monotonic() < deadline:
            with lock:
                complete = received_unique >= total_target and result_count >= args.commands and len(routed) >= args.commands
            if complete:
                try:
                    metrics = json.loads((args.results / "gateway-metrics.json").read_text(encoding="utf-8"))
                    if metrics["mqtt_pipeline"]["telemetry_puback_received"] >= total_target:
                        break
                except (OSError, KeyError, ValueError):
                    pass
            time.sleep(0.2)
    except Exception as exc:
        run_error = str(exc)
    finally:
        if gateway is not None and gateway.poll() is None:
            gateway.send_signal(signal.SIGTERM)
            try:
                gateway.wait(timeout=10)
            except subprocess.TimeoutExpired:
                gateway.kill()
                gateway.wait(timeout=5)
        if gateway_log:
            gateway_log.close()
        monitor_stop.set()
        if monitor:
            monitor.close()
        if monitor_thread:
            monitor_thread.join(timeout=2)
        if sender:
            sender.close()
        for client in (publisher, subscriber):
            if client:
                client.disconnect()
                client.loop_stop()

    try:
        metrics = json.loads((args.results / "gateway-metrics.json").read_text(encoding="utf-8"))
    except (OSError, ValueError):
        metrics = {}
    pipeline = metrics.get("mqtt_pipeline", {})
    missing = [seq for seq in range(1, total_target + 1) if not seen[seq]]
    duration = (end_mono - start_mono) if end_mono is not None and start_mono is not None else None
    gateway_text = (args.results / "gateway.log").read_text(encoding="utf-8") if (args.results / "gateway.log").exists() else ""
    connection_count = gateway_text.count("MQTT connected result=0")
    success = (
        run_error is None and duration is not None and duration >= args.duration
        and generated == total_target and received_unique == total_target and not missing
        and metrics.get("publish_failures") == 0 and metrics.get("rejected") == 0
        and pipeline.get("telemetry_accepted") == total_target
        and pipeline.get("telemetry_puback_received") == total_target
        and commands_sent == args.commands and len(routed) == args.commands
        and result_count == args.commands and result_failures == 0 and not anomalies
        and gateway.returncode == 0
    )
    summary = {
        "result": "PASS" if success else "FAIL", "error": run_error,
        "start_utc": start_wall, "end_utc": end_wall, "duration_seconds": duration,
        "target_rate_msg_s": args.rate, "target_duration_seconds": args.duration,
        "average_generated_rate_msg_s": generated / duration if duration else None,
        "generated": generated,
        "gateway_publish_attempts_message_level": metrics.get("telemetry_dequeued"),
        "gateway_publish_success_api_accepted": pipeline.get("telemetry_accepted"),
        "gateway_publish_success_puback": pipeline.get("telemetry_puback_received"),
        "subscriber_received_total": received_total,
        "subscriber_received_unique": received_unique,
        "duplicates": len(duplicates), "missing_seq_count": len(missing),
        "out_of_order": out_of_order, "invalid_messages": len(anomalies),
        "publish_failures": metrics.get("publish_failures"),
        "queue_rejected": metrics.get("rejected"),
        "commands_sent": commands_sent, "commands_routed": len(routed),
        "command_results_received": result_count,
        "command_missing": sorted(set(range(1, args.commands + 1)) - routed),
        "command_duplicates": max(0, result_count - args.commands),
        "command_failures": result_failures,
        "gateway_reconnect_count": max(0, connection_count - 1),
        "subscriber_reconnect_count": max(0, subscriber_connections - 1),
        "telemetry_topic": telemetry_topic, "command_topic": command_topic,
        "command_result_topic": result_topic, "run_id": run_id,
        "note": "CAN frame payload is 4-byte run id plus 4-byte big-endian seq; gateway JSON retains it as hex. Command results have no command_id field, so result counts correlate by one command per interval and matching CAN frames.",
    }
    save_json(args.results / "final-summary.json", summary)
    save_json(args.results / "uplink-summary.txt", {key: summary[key] for key in (
        "start_utc", "end_utc", "duration_seconds", "target_rate_msg_s", "average_generated_rate_msg_s",
        "generated", "gateway_publish_attempts_message_level", "gateway_publish_success_api_accepted",
        "gateway_publish_success_puback", "subscriber_received_total", "subscriber_received_unique",
        "duplicates", "missing_seq_count", "out_of_order", "invalid_messages", "publish_failures",
        "gateway_reconnect_count", "subscriber_reconnect_count")})
    save_json(args.results / "uplink-samples.txt", {"first_20": samples_first, "last_20": list(samples_last)})
    save_text(args.results / "missing-seq.txt", "empty\n" if not missing else "\n".join(map(str, missing)) + "\n")
    save_text(args.results / "duplicate-seq.txt", "empty\n" if not duplicates else "\n".join(map(str, duplicates)) + "\n")
    save_json(args.results / "anomalies.txt", anomalies)
    save_json(args.results / "downlink-summary.txt", {key: summary[key] for key in (
        "commands_sent", "commands_routed", "command_results_received", "command_missing",
        "command_duplicates", "command_failures", "command_topic", "command_result_topic")})
    save_json(args.results / "command-results.txt", result_samples)
    save_text(args.results / "final-summary.txt", "\n".join(f"{key}={value}" for key, value in summary.items()) + "\n")
    print(json.dumps({key: summary[key] for key in (
        "result", "duration_seconds", "generated", "gateway_publish_success_puback",
        "subscriber_received_unique", "missing_seq_count", "duplicates", "commands_sent", "commands_routed",
        "command_results_received", "publish_failures", "error")}, ensure_ascii=False), flush=True)
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Deterministic Modbus RTU slave with one-shot fault injection."""

import argparse
import csv
import datetime as dt
import select
import struct
import time
from pathlib import Path

import serial


def crc16(data: bytes) -> int:
    crc = 0xFFFF
    for byte in data:
        crc ^= byte
        for _ in range(8):
            crc = (crc >> 1) ^ 0xA001 if crc & 1 else crc >> 1
    return crc


def with_crc(data: bytes) -> bytes:
    return data + struct.pack("<H", crc16(data))


def request_length(buffer: bytes) -> int | None:
    if len(buffer) < 2:
        return None
    function = buffer[1]
    if function in (1, 3, 4, 5, 6):
        return 8
    if function in (15, 16) and len(buffer) >= 7:
        return 9 + buffer[6]
    return 8


class DataStore:
    def __init__(self) -> None:
        self.coils = [False] * 256
        self.holding = [0] * 256
        self.input = [0] * 256
        self.holding[0:4] = [100, 200, 300, 400]
        self.input[0:4] = [1100, 1200, 1300, 1400]

    def handle(self, frame: bytes) -> tuple[bytes, int, int, int]:
        unit, function = frame[0], frame[1]
        address, count_or_value = struct.unpack(">HH", frame[2:6])

        if function == 1:
            count = count_or_value
            packed = bytearray((count + 7) // 8)
            for index in range(count):
                if self.coils[address + index]:
                    packed[index // 8] |= 1 << (index % 8)
            return bytes([unit, function, len(packed)]) + bytes(packed), address, count, function
        if function in (3, 4):
            count = count_or_value
            source = self.holding if function == 3 else self.input
            values = source[address : address + count]
            payload = b"".join(struct.pack(">H", value) for value in values)
            return bytes([unit, function, len(payload)]) + payload, address, count, function
        if function == 5:
            self.coils[address] = count_or_value == 0xFF00
            return frame[:6], address, 1, function
        if function == 6:
            self.holding[address] = count_or_value
            return frame[:6], address, 1, function
        if function == 15:
            count = count_or_value
            values = frame[7 : 7 + frame[6]]
            for index in range(count):
                self.coils[address + index] = bool(values[index // 8] & (1 << (index % 8)))
            return bytes([unit, function]) + struct.pack(">HH", address, count), address, count, function
        if function == 16:
            count = count_or_value
            for index in range(count):
                offset = 7 + index * 2
                self.holding[address + index] = struct.unpack(">H", frame[offset : offset + 2])[0]
            return bytes([unit, function]) + struct.pack(">HH", address, count), address, count, function
        return bytes([unit, function | 0x80, 1]), address, 0, function


def consume_fault(path: Path) -> str:
    try:
        fault = path.read_text(encoding="utf-8").strip() or "normal"
    except FileNotFoundError:
        return "normal"
    if fault != "normal":
        path.write_text("normal\n", encoding="utf-8")
    return fault


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--fault-file", required=True)
    parser.add_argument("--metrics", required=True)
    parser.add_argument("--baud", type=int, default=19200)
    args = parser.parse_args()

    fault_path = Path(args.fault_file)
    fault_path.write_text("normal\n", encoding="utf-8")
    metrics_path = Path(args.metrics)
    metrics_path.parent.mkdir(parents=True, exist_ok=True)
    new_file = not metrics_path.exists()
    metrics = metrics_path.open("a", newline="", encoding="utf-8", buffering=1)
    writer = csv.writer(metrics)
    if new_file:
        writer.writerow(["timestamp_utc", "function", "address", "count", "fault", "request_hex", "response_hex", "request_crc_valid"])

    store = DataStore()
    port = serial.Serial(args.device, baudrate=args.baud, bytesize=8, parity="N", stopbits=1, timeout=0)
    buffer = bytearray()
    while True:
        ready, _, _ = select.select([port.fileno()], [], [], 0.5)
        if not ready:
            continue
        buffer.extend(port.read(256))
        while True:
            size = request_length(buffer)
            if size is None or len(buffer) < size:
                break
            frame = bytes(buffer[:size])
            del buffer[:size]
            crc_valid = len(frame) >= 4 and crc16(frame[:-2]) == struct.unpack("<H", frame[-2:])[0]
            if not crc_valid:
                writer.writerow([dt.datetime.now(dt.timezone.utc).isoformat(), -1, -1, -1, "bad_request_crc", frame.hex(), "", False])
                continue

            body, address, count, function = store.handle(frame)
            fault = consume_fault(fault_path)
            response = with_crc(body)
            if fault == "timeout":
                response = b""
                time.sleep(0.5)
            elif fault == "crc":
                response = response[:-1] + bytes([response[-1] ^ 0xFF])
            elif fault == "invalid":
                response = b"\x7f\x00\x00"
            if response:
                port.write(response)
                port.flush()
            writer.writerow([
                dt.datetime.now(dt.timezone.utc).isoformat(),
                function,
                address,
                count,
                fault,
                frame.hex(),
                response.hex(),
                True,
            ])


if __name__ == "__main__":
    main()

#!/usr/bin/env python3
"""Functional and fault-injection checks for the RTU simulator."""

import argparse
import json
import time
from pathlib import Path

from pymodbus.client import ModbusSerialClient


def check(condition: bool, message: str) -> None:
    if not condition:
        raise AssertionError(message)


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--device", required=True)
    parser.add_argument("--fault-file", required=True)
    parser.add_argument("--summary", required=True)
    args = parser.parse_args()

    client = ModbusSerialClient(
        port=args.device,
        baudrate=19200,
        parity="N",
        stopbits=1,
        bytesize=8,
        timeout=0.25,
        retries=0,
    )
    check(client.connect(), "failed to open virtual RTU serial device")
    checks: dict[str, bool] = {}
    started = time.monotonic()

    result = client.read_holding_registers(0, count=4, slave=1)
    checks["holding_read"] = not result.isError() and result.registers == [100, 200, 300, 400]
    check(checks["holding_read"], f"holding read failed: {result}")

    result = client.read_input_registers(0, count=4, slave=1)
    checks["input_read"] = not result.isError() and result.registers == [1100, 1200, 1300, 1400]
    check(checks["input_read"], f"input read failed: {result}")

    result = client.write_register(0, 4321, slave=1)
    verify = client.read_holding_registers(0, count=1, slave=1)
    checks["holding_write"] = not result.isError() and verify.registers == [4321]
    check(checks["holding_write"], f"holding write failed: {result} / {verify}")

    result = client.write_coil(3, True, slave=1)
    verify = client.read_coils(3, count=1, slave=1)
    checks["coil_write_read"] = not result.isError() and verify.bits[0] is True
    check(checks["coil_write_read"], f"coil write/read failed: {result} / {verify}")

    fault_path = Path(args.fault_file)
    for fault in ("timeout", "crc", "invalid"):
        fault_path.write_text(fault + "\n", encoding="utf-8")
        result = client.read_holding_registers(0, count=1, slave=1)
        checks[f"fault_{fault}_detected"] = result.isError()
        check(checks[f"fault_{fault}_detected"], f"{fault} fault was not detected: {result}")
        client.close()
        check(client.connect(), f"reconnect failed after {fault}")

    result = client.read_holding_registers(0, count=1, slave=1)
    checks["recovery"] = not result.isError() and result.registers == [4321]
    check(checks["recovery"], f"normal operation did not recover: {result}")
    client.close()

    summary = {
        "duration_s": round(time.monotonic() - started, 6),
        "checks_total": len(checks),
        "checks_passed": sum(checks.values()),
        "checks_failed": len(checks) - sum(checks.values()),
        "checks": checks,
    }
    Path(args.summary).write_text(json.dumps(summary, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary, indent=2))


if __name__ == "__main__":
    main()

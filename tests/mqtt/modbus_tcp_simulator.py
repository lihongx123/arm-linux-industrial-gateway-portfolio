#!/usr/bin/env python3
"""Small real Modbus/TCP data store used by the MQTT gateway round-trip test."""

import argparse

from pymodbus.datastore import (
    ModbusSequentialDataBlock,
    ModbusServerContext,
    ModbusSlaveContext,
)
from pymodbus.server import StartTcpServer


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=15020)
    args = parser.parse_args()

    store = ModbusSlaveContext(
        di=ModbusSequentialDataBlock(0, [0] * 128),
        co=ModbusSequentialDataBlock(0, [0] * 128),
        hr=ModbusSequentialDataBlock(0, [0] * 128),
        ir=ModbusSequentialDataBlock(0, [0] * 128),
        zero_mode=True,
    )
    context = ModbusServerContext(slaves=store, single=True)
    StartTcpServer(context=context, address=(args.host, args.port))


if __name__ == "__main__":
    main()

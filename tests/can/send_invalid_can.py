#!/usr/bin/env python3
"""Inject CAN error and RTR frames through a real SocketCAN interface."""

import argparse
import socket
import struct


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--interface", default="vcan0")
    args = parser.parse_args()
    can_socket = socket.socket(socket.AF_CAN, socket.SOCK_RAW, socket.CAN_RAW)
    can_socket.bind((args.interface,))
    for can_id in (0x20000001, 0x40000001):  # CAN_ERR_FLAG, CAN_RTR_FLAG
        can_socket.send(struct.pack("=IB3x8s", can_id, 0, bytes(8)))
    can_socket.close()


if __name__ == "__main__":
    main()

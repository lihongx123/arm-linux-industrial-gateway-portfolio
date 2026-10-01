#!/usr/bin/env python3
"""Boot the Buildroot ARM64 image, run a guest test, and extract its evidence."""

from __future__ import annotations

import argparse
import os
import pathlib
import shutil
import subprocess
import sys
import time
import tempfile
import hashlib
import json

import pexpect


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output", default="/tmp/mqmgateway-br-output")
    parser.add_argument("--repo", default=str(pathlib.Path(__file__).resolve().parents[1]))
    parser.add_argument("--results", help="Use a new, empty evidence directory instead of the legacy results/arm64 path")
    parser.add_argument("--binary-dir", help="Inject freshly compiled binaries from this CMake build into a private image")
    parser.add_argument("--mode", choices=("tests", "soak", "stress"), default="tests")
    parser.add_argument("--soak-seconds", type=int, default=28800)
    parser.add_argument("--soak-rate", type=int, default=500)
    parser.add_argument("--soak-devices", type=int, default=100)
    parser.add_argument("--soak-queue", type=int, default=1024)
    parser.add_argument("--soak-workers", type=int, default=2)
    parser.add_argument("--stress-seconds", type=int, default=30)
    parser.add_argument("--stress-devices", type=int, default=100)
    parser.add_argument("--stress-queue", type=int, default=1024)
    parser.add_argument("--stress-workers", type=int, default=2)
    args = parser.parse_args()

    output = pathlib.Path(args.output).resolve()
    repo = pathlib.Path(args.repo).resolve()
    images = output / "images"
    result_name = {"tests": "qemu", "soak": "soak", "stress": "stress"}[args.mode]
    if args.results:
        result_root = pathlib.Path(args.results).resolve()
        if result_root.exists() and any(result_root.iterdir()):
            raise RuntimeError(f"refusing to overwrite non-empty results directory: {result_root}")
        result_root.mkdir(parents=True, exist_ok=True)
    else:
        result_root = repo / "results" / "arm64" / result_name
        result_root.mkdir(parents=True, exist_ok=True)
        for old in result_root.iterdir():
            if old.is_dir():
                shutil.rmtree(old)
            else:
                old.unlink()

    runtime_image = pathlib.Path(tempfile.mkdtemp(prefix="mqm-validation-")) / "rootfs.ext4"
    shutil.copy2(images / "rootfs.ext4", runtime_image)
    if args.binary_dir:
        build = pathlib.Path(args.binary_dir).resolve()
        files = [build / "src/iot_gateway/mqmgateway_iot",
                 build / "src/serial/mqmgateway_rtu_transport_tests"]
        manifest = {}
        for binary in files:
            if not binary.is_file():
                raise RuntimeError(f"missing new binary: {binary}")
            destination = "/usr/bin/" + binary.name
            subprocess.run(["debugfs", "-w", "-R", "rm "+destination, str(runtime_image)], check=True)
            subprocess.run(["debugfs", "-w", "-R", f"write {binary} {destination}", str(runtime_image)], check=True)
            manifest[str(binary)] = hashlib.sha256(binary.read_bytes()).hexdigest()
        (result_root/"injected-binaries.json").write_text(json.dumps(manifest, indent=2))
    qemu = shutil.which("qemu-system-aarch64")
    if not qemu:
        raise RuntimeError("qemu-system-aarch64 is not installed")

    command = [
        qemu,
        "-M", "virt",
        "-cpu", "cortex-a57",
        "-smp", "2",
        "-m", "1024",
        "-kernel", str(images / "Image"),
        "-drive", f"file={runtime_image},if=none,format=raw,id=hd0",
        "-device", "virtio-blk-device,drive=hd0",
        "-append", "rootwait root=/dev/vda console=ttyAMA0",
        "-netdev", "user,id=eth0",
        "-device", "virtio-net-device,netdev=eth0",
        "-nographic",
        "-no-reboot",
    ]
    console_path = result_root / "qemu-console.log"
    started = time.monotonic()
    if args.mode == "soak":
        timeout = max(900, args.soak_seconds + 900)
    elif args.mode == "stress":
        timeout = max(1800, args.stress_seconds * 20 + 1200)
    else:
        timeout = 900
    test_ok = False
    guest_completed = False
    failure_reason = ""
    with console_path.open("w", encoding="utf-8", errors="replace") as console:
        child = pexpect.spawn(command[0], command[1:], encoding="utf-8", codec_errors="replace", timeout=timeout)
        child.logfile = console
        try:
            child.expect("buildroot login:")
            child.sendline("root")
            child.expect(r"# ")
            if args.mode == "tests":
                guest_command = "/root/run-arm-tests.sh"
            elif args.mode == "soak":
                guest_command = (
                    f"/root/run-arm-soak.sh {args.soak_seconds} {args.soak_rate} "
                    f"{args.soak_devices} {args.soak_queue} {args.soak_workers}"
                )
            else:
                guest_command = (
                    f"/root/run-arm-stress.sh {args.stress_seconds} {args.stress_devices} "
                    f"{args.stress_queue} {args.stress_workers}"
                )
            child.sendline(f"{guest_command}; rc=$?; echo __MQM_RC_${{rc}}__")
            match = child.expect([r"__MQM_RC_0__", r"__MQM_RC_([1-9][0-9]*)__"])
            test_ok = match == 0
            child.expect(r"# ")
            child.sendline("sync; poweroff")
            child.expect(pexpect.EOF)
            guest_completed = True
        except (pexpect.EOF, pexpect.TIMEOUT) as error:
            failure_reason = type(error).__name__
        finally:
            child.close(force=not guest_completed)

    extracted = result_root / "guest-results"
    extracted.mkdir()
    guest_path = {
        "tests": "/root/results",
        "soak": "/root/soak-results",
        "stress": "/root/stress-results",
    }[args.mode]
    debugfs = subprocess.run(
        ["debugfs", "-R", f"rdump {guest_path} {extracted}", str(runtime_image)],
        text=True,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        check=False,
    )
    (result_root / "debugfs-extract.log").write_text(debugfs.stdout, encoding="utf-8")
    elapsed = time.monotonic() - started
    (result_root / "host-run.txt").write_text(
        f"mode={args.mode}\nqemu={qemu}\nqemu_version={subprocess.check_output([qemu, '--version'], text=True).splitlines()[0]}\n"
        f"elapsed_seconds={elapsed:.3f}\nguest_exit={'PASS' if test_ok else 'FAIL'}\n"
        f"debugfs_exit={debugfs.returncode}\nfailure_reason={failure_reason or 'none'}\n",
        encoding="utf-8",
    )
    if not test_ok or debugfs.returncode != 0:
        print(f"QEMU {args.mode}: FAIL; see {console_path}", file=sys.stderr)
        return 1
    print(f"QEMU {args.mode}: PASS ({elapsed:.3f}s), evidence: {result_root}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

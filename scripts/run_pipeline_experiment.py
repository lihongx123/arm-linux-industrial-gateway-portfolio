#!/usr/bin/env python3
"""Isolated, fingerprinted ARM64 diagnostic matrix; never reuse a running image."""
import argparse
import hashlib
import json
import pathlib
import re
import shutil
import subprocess
import tempfile
import time
import pexpect


def sha256(path):
    with open(path, "rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--images", type=pathlib.Path, default=pathlib.Path("/tmp/mqmgateway-br-output/images"))
    parser.add_argument("--gateway", type=pathlib.Path, required=True)
    parser.add_argument("--results", type=pathlib.Path, required=True)
    parser.add_argument("--seconds", type=int, default=30)
    parser.add_argument("--rates", default="1000,1500,2000")
    parser.add_argument("--variants", default="1:0:20,1:1:20,0:1:20", help="qos:observer:inflight combinations")
    parser.add_argument("--broker-inflight", type=int, default=20)
    parser.add_argument("--workers", type=int, default=2)
    parser.add_argument("--queue", type=int, default=1024)
    parser.add_argument("--devices", type=int, default=100)
    parser.add_argument("--vcpus", type=int, default=2)
    parser.add_argument("--memory-mb", type=int, default=1024)
    parser.add_argument("--sample-processes", action="store_true")
    args = parser.parse_args()
    if not (1 <= args.workers <= 32 and 1 <= args.queue <= 65536 and
            1 <= args.devices <= 2047 and 1 <= args.vcpus <= 8 and
            256 <= args.memory_mb <= 4096):
        parser.error("workers 1..32, queue 1..65536, devices 1..2047, vcpus 1..8, memory 256..4096 required")
    if args.seconds <= 0 or not re.fullmatch(r"[1-9][0-9]*(,[1-9][0-9]*)*", args.rates):
        parser.error("positive duration and comma-separated positive rates required")
    if not re.fullmatch(r"[01]:[01]:[1-9][0-9]*(,[01]:[01]:[1-9][0-9]*)*", args.variants):
        parser.error("variants must be qos:observer:inflight triples")
    if any(int(v.split(":")[2]) > 65535 for v in args.variants.split(",")):
        parser.error("inflight exceeds 65535")
    if not 1 <= args.broker_inflight <= 65535:
        parser.error("broker-inflight must be 1..65535")
    if len(set(args.rates.split(","))) != len(args.rates.split(",")) or len(set(args.variants.split(","))) != len(args.variants.split(",")):
        parser.error("use separate run directories for repetitions, not duplicate case names")
    repo = pathlib.Path(__file__).resolve().parents[1]
    # Excludes pgrep itself (no QEMU name in this process command line).
    active = subprocess.run(["pgrep", "-f", "[/]usr/bin/qemu-system-aarch64"], capture_output=True, text=True)
    if active.returncode == 0:
        raise RuntimeError("A QEMU guest is active; do not contaminate concurrent capacity tests")
    args.results.mkdir(parents=True, exist_ok=False)
    image = pathlib.Path(tempfile.mkdtemp(prefix="mqm-pipeline-runtime-")) / "runtime.ext4"
    shutil.copyfile(args.images / "rootfs.ext4", image)
    script = repo / "buildroot/overlay/root/run-arm-pipeline.sh"
    metadata = {
        "started_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "duration_per_case": args.seconds, "rates": args.rates,
        "variants": args.variants,
        "broker_inflight": args.broker_inflight,
        "qemu_vcpus": args.vcpus, "qemu_memory_mb": args.memory_mb,
        "workers": args.workers, "queue": args.queue, "devices": args.devices,
        "sample_processes": args.sample_processes,
        "base_image_sha256": sha256(args.images / "rootfs.ext4"),
        "kernel_sha256": sha256(args.images / "Image"),
        "gateway_sha256": sha256(args.gateway), "guest_script_sha256": sha256(script),
        "qemu_version": subprocess.check_output(["qemu-system-aarch64", "--version"], text=True).splitlines()[0],
        "source_sha256": {str(p.relative_to(repo)): sha256(p) for p in sorted((repo / "src/iot_gateway").glob("*")) if p.is_file()},
        "host_uname": subprocess.check_output(["uname", "-a"], text=True).strip(),
        "runtime_image": str(image),
    }
    def debugfs(request, writable=False):
        command = ["debugfs"] + (["-w"] if writable else []) + ["-R", request, str(image)]
        result = subprocess.run(command, text=True, capture_output=True)
        with (args.results / "image-operations.log").open("a") as log:
            log.write(request + "\n" + result.stdout + result.stderr)
        if result.returncode: raise RuntimeError("debugfs failed: " + request)
    # Use paths without whitespace in debugfs commands. Copy bytes only; the
    # original rootfs, Buildroot output, old binaries and raw evidence stay intact.
    with tempfile.TemporaryDirectory(prefix="mqm-pipeline-") as staging:
        gateway = pathlib.Path(staging) / "gateway"
        guest_script = pathlib.Path(staging) / "runner.sh"
        shutil.copy2(args.gateway, gateway)
        shutil.copy2(script, guest_script)
        debugfs("rm /usr/bin/mqmgateway_iot", True)
        debugfs(f"write {gateway} /usr/bin/mqmgateway_iot", True)
        debugfs("set_inode_field /usr/bin/mqmgateway_iot mode 0100755", True)
        debugfs(f"write {guest_script} /root/run-arm-pipeline.sh", True)
        debugfs("set_inode_field /root/run-arm-pipeline.sh mode 0100755", True)
        sampler = repo / "buildroot/overlay/root/sample-pipeline-processes.sh"
        staged_sampler = pathlib.Path(staging) / "sampler.sh"
        shutil.copyfile(sampler, staged_sampler)
        debugfs(f"write {staged_sampler} /root/sample-pipeline-processes.sh", True)
        metadata["sampler_sha256"] = sha256(sampler)
        original_probe = pathlib.Path(staging) / "probe"
        debugfs(f"dump /usr/bin/mqmgateway_arm64_stress {original_probe}")
        metadata["probe_sha256"] = sha256(original_probe)
        verify_gateway = pathlib.Path(staging) / "gateway-readback"
        debugfs(f"dump /usr/bin/mqmgateway_iot {verify_gateway}")
        if sha256(verify_gateway) != metadata["gateway_sha256"]:
            raise RuntimeError("gateway image readback mismatch")
    command = ["/usr/bin/qemu-system-aarch64", "-M", "virt", "-cpu", "cortex-a57", "-smp", str(args.vcpus), "-m", str(args.memory_mb),
               "-kernel", str(args.images / "Image"), "-drive", f"file={image},if=none,format=raw,id=hd0",
               "-device", "virtio-blk-device,drive=hd0", "-append", "rootwait root=/dev/vda console=ttyAMA0",
               "-netdev", "user,id=eth0", "-device", "virtio-net-device,netdev=eth0", "-nographic", "-no-reboot"]
    metadata["command"] = command
    (args.results / "manifest.json").write_text(json.dumps(metadata, indent=2))
    started = time.monotonic()
    timeout = len(args.rates.split(",")) * len(args.variants.split(",")) * (args.seconds + 30) + 300
    status = "INFRASTRUCTURE_ERROR"
    failure = ""
    with (args.results / "qemu-console.log").open("w") as log:
        child = pexpect.spawn(command[0], command[1:], encoding="utf-8", codec_errors="replace", timeout=timeout)
        child.logfile = log
        try:
            child.expect("buildroot login:")
            child.sendline("root")
            child.expect(r"# ")
            child.sendline(f"/root/run-arm-pipeline.sh {args.seconds} {args.rates} {args.variants} {args.broker_inflight} {args.workers} {args.queue} {args.devices} {int(args.sample_processes)}; rc=$?; echo __PIPELINE_RC_${{rc}}__")
            match = child.expect(["__PIPELINE_RC_0__", r"__PIPELINE_RC_([1-9][0-9]*)__"])
            status = "MATRIX_COMPLETED" if match == 0 else "GUEST_ERROR"
            child.expect(r"# ")
            child.sendline("sync; poweroff")
            child.expect(pexpect.EOF)
        except (pexpect.EOF, pexpect.TIMEOUT) as error:
            failure = type(error).__name__
            status = "INFRASTRUCTURE_ERROR"
        finally:
            child.close(force=True)
    extracted = args.results.resolve() / "guest-results"
    extracted.mkdir()
    debugfs(f"rdump /root/pipeline-results {extracted}")
    expected = len(args.rates.split(",")) * len(args.variants.split(","))
    actual = len(list((extracted / "pipeline-results").glob("*/result.json")))
    if status == "MATRIX_COMPLETED" and actual != expected:
        status = "EVIDENCE_ERROR"
        failure = f"expected {expected} result files, extracted {actual}"
    (args.results / "host-run.json").write_text(json.dumps({"status": status, "failure": failure,
        "elapsed_seconds": time.monotonic() - started}, indent=2))
    print(status, args.results)
    return 0 if status == "MATRIX_COMPLETED" else 1


if __name__ == "__main__":
    raise SystemExit(main())

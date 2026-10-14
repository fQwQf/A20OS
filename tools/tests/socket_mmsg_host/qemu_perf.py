#!/usr/bin/env python3
"""Exclusive alternating RV64 mmsg throughput sample on frozen C/Rust kernels."""
from __future__ import annotations
import argparse
import csv
import fcntl
import hashlib
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import time

REPO = Path(__file__).resolve().parents[3]
METRIC = re.compile(r"MMSG_BENCH: batch=(\d+) loops=(\d+) elapsed_ns=(\d+) "
                    r"messages_per_sec=(\d+) ns_per_send_or_recv=(\d+)")


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def required_file(value: str) -> Path:
    path = Path(value)
    if not path.is_absolute() or not path.is_file():
        raise ValueError(f"expected existing absolute file: {value}")
    return path.resolve()


def lock(path: Path, exclusive: bool) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    fcntl.flock(fd, fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH)
    return fd


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", required=True)
    ap.add_argument("--source-manifest", required=True)
    ap.add_argument("--user-manifest", required=True)
    ap.add_argument("--benchmark-elf", required=True)
    ap.add_argument("--c-kernel", required=True)
    ap.add_argument("--rust-kernel", required=True)
    ap.add_argument("--c-image", required=True)
    ap.add_argument("--rust-image", required=True)
    ap.add_argument("--boot-delay", type=float, default=8)
    ap.add_argument("--timeout", type=float, default=120)
    args = ap.parse_args()
    out = Path(args.outdir)
    if not out.is_absolute():
        ap.error("--outdir must be absolute")
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        out.mkdir()
    except FileExistsError:
        ap.error(f"refusing to overwrite evidence: {out}")
    source_manifest = required_file(args.source_manifest)
    user_manifest = required_file(args.user_manifest)
    benchmark = required_file(args.benchmark_elf)
    controls = {
        "c": (required_file(args.c_kernel), required_file(args.c_image)),
        "rust": (required_file(args.rust_kernel), required_file(args.rust_image)),
    }
    if controls["c"][1] == controls["rust"][1]:
        ap.error("C and Rust must use distinct private image paths")
    common = Path(subprocess.check_output(
        ["git", "-C", str(REPO), "rev-parse", "--path-format=absolute", "--git-common-dir"],
        text=True).strip())
    activity = lock(common / "parallel-rewrite" / "activity.lock", True)
    slot = None
    for name in ("qemu-1.lock", "qemu-2.lock"):
        fd = os.open(common / "parallel-rewrite" / name, os.O_CREAT | os.O_RDWR, 0o600)
        try:
            fcntl.flock(fd, fcntl.LOCK_EX | fcntl.LOCK_NB)
            slot = (name, fd)
            break
        except BlockingIOError:
            os.close(fd)
    if slot is None:
        os.close(activity)
        ap.error("no free QEMU slot")
    qemu = shutil.which("qemu-system-riscv64")
    if not qemu:
        ap.error("qemu-system-riscv64 unavailable")
    qemu_version = subprocess.check_output([qemu, "--version"], text=True).splitlines()[0]
    manifest = {
        "qemu_version": qemu_version,
        "activity_lock": "exclusive",
        "qemu_slot": slot[0],
        "guest_memory": "1G",
        "pair_order": [],
        "source_manifest": str(source_manifest),
        "source_manifest_sha256": sha(source_manifest),
        "user_manifest": str(user_manifest),
        "user_manifest_sha256": sha(user_manifest),
        "benchmark_elf": str(benchmark),
        "benchmark_elf_sha256": sha(benchmark),
        "runner": str(Path(__file__).resolve()),
        "runner_sha256": sha(Path(__file__).resolve()),
        "boot_delay_seconds": args.boot_delay,
        "timeout_seconds": args.timeout,
        "samples": [],
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    order = []
    for pair in range(5):
        order.extend(([("c", pair, 0), ("rust", pair, 1)] if pair % 2 == 0
                      else [("rust", pair, 0), ("c", pair, 1)]))
    rows = []
    all_pass = True
    for serial, (impl, pair, position) in enumerate(order, 1):
        kernel, image = controls[impl]
        before = {"kernel": sha(kernel), "image": sha(image), "benchmark": sha(benchmark)}
        run = out / f"pair-{pair + 1}-{impl}"
        run.mkdir()
        argv = [qemu, "-machine", "virt", "-m", "1G", "-nographic", "-smp", "1",
                "-bios", "default", "-global", "virtio-mmio.force-legacy=false",
                "-snapshot", "-drive", f"file={image},if=none,format=raw,id=x0",
                "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
                "-netdev", "user,id=net", "-device",
                "virtio-net-device,netdev=net,bus=virtio-mmio-bus.4",
                "-kernel", str(kernel), "-append",
                "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
                "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os"]
        (run / "argv.json").write_text(json.dumps(argv, indent=2) + "\n")
        (run / "inputs.sha256").write_text(
            f"{before['kernel']}  {kernel}\n{before['image']}  {image}\n"
            f"{before['benchmark']}  {benchmark}\n")
        started = time.time()
        with (run / "qemu.raw.log").open("wb") as log:
            proc = subprocess.Popen(argv, cwd=REPO, stdin=subprocess.PIPE,
                                    stdout=log, stderr=subprocess.STDOUT)
            time.sleep(args.boot_delay)
            timed_out = False
            try:
                assert proc.stdin is not None
                proc.stdin.write(b"mmsg_bench\npoweroff\n")
                proc.stdin.flush()
                proc.stdin.close()
                status = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                proc.kill()
                status = proc.wait()
        raw = (run / "qemu.raw.log").read_text(errors="replace")
        metrics = [dict(zip(("batch", "loops", "elapsed_ns", "messages_per_sec",
                             "ns_per_send_or_recv"), map(int, match)))
                   for match in METRIC.findall(raw)]
        after = {"kernel": sha(kernel), "image": sha(image), "benchmark": sha(benchmark)}
        expected_batches = {1, 8}
        passed = (status == 0 and not timed_out and "MMSG_BENCH: PASS" in raw and
                  "MMSG_BENCH: FAIL" not in raw and
                  {row["batch"] for row in metrics} == expected_batches and before == after)
        sample = {
            "serial": serial, "pair": pair + 1, "position": position,
            "implementation": impl, "actual_process_exit": status,
            "timed_out": timed_out, "duration_seconds": time.time() - started,
            "input_hashes_before": before, "input_hashes_after": after,
            "metrics": metrics, "passed": passed,
        }
        (run / "result.json").write_text(json.dumps(sample, indent=2) + "\n")
        rows.extend({"pair": pair + 1, "implementation": impl, **metric} for metric in metrics)
        manifest["pair_order"].append({"pair": pair + 1, "implementation": impl,
                                       "position": position})
        manifest["samples"].append(sample)
        all_pass &= passed
        (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
        print(f"pair={pair + 1} impl={impl}: {'PASS' if passed else 'FAIL'} "
              f"exit={status} metrics={len(metrics)}", flush=True)
    with (out / "samples.csv").open("w", newline="") as stream:
        writer = csv.DictWriter(stream, fieldnames=("pair", "implementation", "batch",
            "loops", "elapsed_ns", "messages_per_sec", "ns_per_send_or_recv"))
        writer.writeheader()
        writer.writerows(rows)
    summary = {}
    for impl in ("c", "rust"):
        summary[impl] = {}
        for batch in (1, 8):
            values = [r["messages_per_sec"] for r in rows
                      if r["implementation"] == impl and r["batch"] == batch]
            values.sort()
            summary[impl][str(batch)] = {
                "samples": values,
                "median_messages_per_sec": values[len(values) // 2] if values else None,
                "min_messages_per_sec": min(values) if values else None,
                "max_messages_per_sec": max(values) if values else None,
            }
    manifest["summary"] = summary
    manifest["overall_pass"] = all_pass
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    os.close(slot[1])
    os.close(activity)
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())

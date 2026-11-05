#!/usr/bin/env python3
"""Interleaved end-to-end C/R timing for an MM selector pair.

With --c-dir/--rust-dir this compares any two build directories of the same
profile, so it serves the mapping-creation selector and the unmap selector
alike; without them it compares the retained C-reference mapping-creation
kernel against the Rust one.

It compares the retained C-reference kernel (the last build with MMAP_IMPL=c,
commit 74880b29) against the Rust kernel.  Both are built first (that is the
smoke cases' job); this script only boots guests, so it takes the perf lease:
activity.lock exclusively and qemu-1.lock exclusively.  It must not overlap a
build or another guest.

The order is C, R, R, C, so a host-load drift during the run cannot favour one
selector: each implementation is measured once early and once late.  Each
guest is fed `mm_stress` as soon as its shell prompt appears and `poweroff`
immediately after MM_STRESS: PASS, so the measured interval is the workload,
not a fixed sleep.  Every run records the exact QEMU argv, the kernel and
input hashes, the natural exit status and the raw log.

Usage: tools/bench-mmap-selector.py [--runs-per-order 1] [--out DIR]
"""

from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent.parent
LOCKDIR = Path(subprocess.run(
    ["git", "rev-parse", "--path-format=absolute", "--git-common-dir"],
    cwd=REPO, capture_output=True, text=True, check=True).stdout.strip()
) / "parallel-rewrite"

DEFAULT_BUILD_DIRS = {"c": ".kernel-build/evidence/mmap-c",
                      "rust": ".kernel-build/evidence/mmap-rust"}
BUILD_DIRS = dict(DEFAULT_BUILD_DIRS)


def sha256(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def qemu_argv(build_dir: str) -> list[str]:
    return [
        "qemu-system-riscv64", "-machine", "virt", "-m", "1G", "-nographic",
        "-smp", "2", "-bios", "default",
        "-global", "virtio-mmio.force-legacy=false",
        "-drive", f"file={build_dir}/fat32.img,if=none,format=raw,id=x0",
        "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
        "-netdev", "user,id=net",
        "-device", "virtio-net-device,netdev=net,bus=virtio-mmio-bus.4",
        "-kernel", f"{build_dir}/kernel.elf",
    ]


def run_one(impl: str, log_path: Path, cmd: str, marker: str,
            timeout_s: float = 240.0) -> dict:
    build_dir = BUILD_DIRS[impl]
    argv = qemu_argv(build_dir)
    kernel = REPO / build_dir / "kernel.elf"
    log_path.parent.mkdir(parents=True, exist_ok=True)
    start = time.monotonic()
    with log_path.open("wb") as fh:
        proc = subprocess.Popen(argv, cwd=REPO, stdin=subprocess.PIPE,
                                stdout=fh, stderr=subprocess.STDOUT)
        text = ""
        sent_stress = sent_poweroff = None
        workload_s = None
        status = None
        try:
            while True:
                if time.monotonic() - start > timeout_s:
                    proc.kill()
                    status = proc.wait()
                    break
                try:
                    proc.stdin.flush()
                except (BrokenPipeError, ValueError, OSError):
                    pass
                if proc.poll() is not None:
                    status = proc.returncode
                    break
                time.sleep(0.05)
                try:
                    text = log_path.read_text(errors="replace")
                except OSError:
                    continue
                if sent_stress is None and "\n# " in text:
                    proc.stdin.write(f"{cmd}\n".encode())
                    sent_stress = time.monotonic()
                elif (sent_stress is not None and sent_poweroff is None
                      and marker in text):
                    workload_s = time.monotonic() - sent_stress
                    proc.stdin.write(b"poweroff\n")
                    sent_poweroff = time.monotonic()
        finally:
            try:
                proc.stdin.close()
            except (BrokenPipeError, ValueError, OSError):
                pass
        if status is None:
            status = proc.wait()
    e2e_s = time.monotonic() - start
    text = log_path.read_text(errors="replace")
    return {
        "impl": impl,
        "argv": argv,
        "kernel_sha256": sha256(kernel),
        "exit_status": status,
        "workload_s": None if workload_s is None else round(workload_s, 3),
        "end_to_end_s": round(e2e_s, 3),
        "marker_seen": marker in text,
        "poweroff": "System is going down for power-off NOW" in text,
        "panic": any(m in text for m in ("Kernel panic", "PANIC", "LOCK-STALL")),
        "log": str(log_path.relative_to(REPO)),
        "log_sha256": sha256(log_path),
    }


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--runs-per-order", type=int, default=1)
    ap.add_argument("--out", default=".kernel-build/evidence/mmap-perf")
    ap.add_argument("--marker", default="MM_STRESS: PASS",
                    help="completion marker in the guest log")
    ap.add_argument("--cmd", default="mm_stress",
                    help="guest command to time (default: mm_stress)")
    ap.add_argument("--c-dir", help="build directory of the C reference kernel")
    ap.add_argument("--rust-dir", help="build directory of the Rust kernel")
    a = ap.parse_args()
    if a.c_dir:
        BUILD_DIRS["c"] = a.c_dir
    if a.rust_dir:
        BUILD_DIRS["rust"] = a.rust_dir
    for impl, d in BUILD_DIRS.items():
        for f in ("kernel.elf", "fat32.img"):
            if not (REPO / d / f).exists():
                print(f"bench-mmap-selector: {impl} build is missing {d}/{f}",
                      file=sys.stderr)
                return 2

    activity = (LOCKDIR / "activity.lock").open("r+")
    qemu = (LOCKDIR / "qemu-1.lock").open("r+")
    fcntl.flock(activity, fcntl.LOCK_EX)
    try:
        fcntl.flock(qemu, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print("bench-mmap-selector: qemu slot busy", file=sys.stderr)
        return 75

    out_dir = REPO / a.out
    out_dir.mkdir(parents=True, exist_ok=True)
    order = ["c", "rust"] * a.runs_per_order
    order += list(reversed(order))
    results = []
    for impl in order:
        n = sum(1 for r in results if r["impl"] == impl) + 1
        r = run_one(impl, out_dir / f"{impl}-run{n}.log", a.cmd, a.marker)
        results.append(r)
        print(f"{impl:>4} run{n}: exit={r['exit_status']} "
              f"workload={r['workload_s']}s e2e={r['end_to_end_s']}s "
              f"pass={r['marker_seen']} panic={r['panic']}", flush=True)

    manifest = {
        "order": order,
        "cmd": a.cmd,
        "input_sha256": sha256(REPO / "user/build/riscv64/mm_stress"),
        "results": results,
    }
    (out_dir / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"bench-mmap-selector: manifest {out_dir / 'manifest.json'}")

    bad = [r for r in results if r["exit_status"] != 0 or not r["marker_seen"]
           or r["panic"]]
    if bad:
        print(f"bench-mmap-selector: FAIL {len(bad)} run(s) did not pass",
              file=sys.stderr)
        return 1
    for impl in ("c", "rust"):
        times = [r["workload_s"] for r in results if r["impl"] == impl]
        print(f"{impl}: workload_s={times}")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
"""Run the frozen socket/event/SCM workload on C and Rust kernel controls.

The caller supplies four private, staged RV64 images and matching kernels.
This runner never builds or edits those inputs. It runs one guest at a time,
under the shared QEMU lease, and retains exact argv, hashes and raw serial logs.
"""
from __future__ import annotations

import argparse
import fcntl
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys
import time

REPO = Path(__file__).resolve().parents[3]
COMMANDS = [
    "socket_stress", "scm_stress", "poll_edge", "io_event_test",
    "signalfd_stress", "timer_edge", "syscall_ext",
]
MARKERS = [
    "SOCKET_STRESS: PASS", "SCM_STRESS: PASS", "POLL_EDGE: PASS",
    "IO_EVENT_TEST: PASS", "SIGNALFD_STRESS: PASS", "TIMER_EDGE: PASS",
    "SYSCALL_EXT: PASS", "System is going down for power-off",
]
FORBIDDEN = ["SOCKET_STRESS: FAIL", "SCM_STRESS: FAIL", "POLL_EDGE: FAIL",
             "IO_EVENT_TEST: FAIL", "SIGNALFD_STRESS: FAIL", "TIMER_EDGE: FAIL",
             "SYSCALL_EXT: FAIL", "FATAL:", "KERNEL PANIC", "kernel panic"]


def sha(path: Path) -> str:
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def required_file(text: str) -> Path:
    p = Path(text)
    if not p.is_absolute() or not p.is_file():
        raise ValueError(f"expected existing absolute file path: {text}")
    return p.resolve()


def acquire(path: Path, exclusive: bool = False) -> int:
    path.parent.mkdir(parents=True, exist_ok=True)
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o600)
    op = fcntl.LOCK_EX if exclusive else fcntl.LOCK_SH
    fcntl.flock(fd, op)
    return fd


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--outdir", required=True, help="new absolute evidence directory")
    ap.add_argument("--source-manifest", required=True)
    ap.add_argument("--user-manifest", required=True)
    ap.add_argument("--native-linux-elf", required=True,
                    help="preserved Native caller ELF, embedded in ABI=both images")
    for key in ("c-linux-kernel", "rust-linux-kernel", "c-both-kernel",
                "rust-both-kernel", "c-linux-image", "rust-linux-image",
                "c-both-image", "rust-both-image"):
        ap.add_argument("--" + key, required=True)
    ap.add_argument("--timeout", type=float, default=180)
    ap.add_argument("--boot-delay", type=float, default=8)
    args = ap.parse_args()

    out = Path(args.outdir)
    if not out.is_absolute():
        ap.error("--outdir must be absolute")
    out.parent.mkdir(parents=True, exist_ok=True)
    try:
        out.mkdir()
    except FileExistsError:
        ap.error(f"refusing to overwrite evidence directory: {out}")

    source_manifest = required_file(args.source_manifest)
    user_manifest = required_file(args.user_manifest)
    native_linux_elf = required_file(args.native_linux_elf)
    pairs = {
        "linux-c": (required_file(args.c_linux_kernel), required_file(args.c_linux_image)),
        "linux-rust": (required_file(args.rust_linux_kernel), required_file(args.rust_linux_image)),
        "both-c": (required_file(args.c_both_kernel), required_file(args.c_both_image)),
        "both-rust": (required_file(args.rust_both_kernel), required_file(args.rust_both_image)),
    }
    if len({str(image) for _, image in pairs.values()}) != 4:
        ap.error("each C/Rust/ABI run must use its own private disk image")

    common = Path(subprocess.check_output(
        ["git", "-C", str(REPO), "rev-parse", "--path-format=absolute", "--git-common-dir"],
        text=True).strip())
    # Keep both descriptors open until every guest has exited and evidence is saved.
    activity_fd = acquire(common / "parallel-rewrite" / "activity.lock")
    qemu_fd = acquire(common / "parallel-rewrite" / "qemu-1.lock")

    qemu = shutil.which("qemu-system-riscv64")
    if not qemu:
        ap.error("qemu-system-riscv64 is unavailable")
    version = subprocess.check_output([qemu, "--version"], text=True).splitlines()[0]
    evidence = {
        "runner": str(Path(__file__).resolve()),
        "runner_sha256": sha(Path(__file__).resolve()),
        "source_manifest": str(source_manifest),
        "source_manifest_sha256": sha(source_manifest),
        "user_manifest": str(user_manifest),
        "user_manifest_sha256": sha(user_manifest),
        "native_linux_elf": str(native_linux_elf),
        "native_linux_elf_sha256": sha(native_linux_elf),
        "qemu_version": version,
        "boot_delay_seconds": args.boot_delay,
        "timeout_seconds": args.timeout,
        "command_lines": COMMANDS,
        "required_markers": MARKERS,
        "forbidden_markers": FORBIDDEN,
        "runs": {},
    }
    (out / "manifest.json").write_text(json.dumps(evidence, indent=2) + "\n")
    all_pass = True
    for name, (kernel, image) in pairs.items():
        before = {"kernel": sha(kernel), "image": sha(image)}
        command_lines = list(COMMANDS)
        required_markers = list(MARKERS)
        if name.startswith("both-"):
            command_lines.append("native-linux-rv")
            required_markers.append("NATIVE_LINUX: PASS")
        command_lines.append("poweroff")
        argv = [qemu, "-machine", "virt", "-m", "1G", "-nographic", "-smp", "1",
                "-bios", "default", "-global", "virtio-mmio.force-legacy=false",
                "-snapshot", "-drive", f"file={image},if=none,format=raw,id=x0",
                "-device", "virtio-blk-device,drive=x0,bus=virtio-mmio-bus.0",
                "-netdev", "user,id=net", "-device",
                "virtio-net-device,netdev=net,bus=virtio-mmio-bus.4",
                "-kernel", str(kernel), "-append",
                "a20.ip=10.0.2.15 a20.netmask=255.255.255.0 "
                "a20.gateway=10.0.2.2 a20.dns=10.0.2.3 a20.hostname=a20os"]
        run_dir = out / name
        run_dir.mkdir()
        (run_dir / "argv.json").write_text(json.dumps(argv, indent=2) + "\n")
        (run_dir / "inputs.sha256").write_text(
            f"{before['kernel']}  {kernel}\n{before['image']}  {image}\n"
            f"{sha(native_linux_elf)}  {native_linux_elf}\n")
        started = time.time()
        raw_path = run_dir / "qemu.raw.log"
        status: int | None = None
        timed_out = False
        with raw_path.open("wb") as log:
            proc = subprocess.Popen(argv, cwd=REPO, stdin=subprocess.PIPE,
                                    stdout=log, stderr=subprocess.STDOUT)
            time.sleep(args.boot_delay)
            try:
                assert proc.stdin is not None
                proc.stdin.write(("".join(line + "\n" for line in command_lines)).encode())
                proc.stdin.flush()
                proc.stdin.close()
                status = proc.wait(timeout=args.timeout)
            except subprocess.TimeoutExpired:
                timed_out = True
                proc.kill()
                status = proc.wait()
            except BrokenPipeError:
                status = proc.wait(timeout=args.timeout)
        raw = raw_path.read_text(errors="replace")
        missing = [marker for marker in required_markers if marker not in raw]
        forbidden = [marker for marker in FORBIDDEN if marker in raw]
        after = {"kernel": sha(kernel), "image": sha(image)}
        passed = status == 0 and not timed_out and not missing and not forbidden and before == after
        all_pass &= passed
        record = {
            "argv": argv,
            "started_unix": started,
            "duration_seconds": time.time() - started,
            "actual_process_exit": status,
            "timed_out": timed_out,
            "missing_markers": missing,
            "command_lines": command_lines,
            "required_markers": required_markers,
            "forbidden_markers_found": forbidden,
            "input_hashes_before": before,
            "input_hashes_after": after,
            "passed": passed,
        }
        (run_dir / "result.json").write_text(json.dumps(record, indent=2) + "\n")
        evidence["runs"][name] = {k: v for k, v in record.items() if k != "argv"}
        (out / "manifest.json").write_text(json.dumps(evidence, indent=2) + "\n")
        print(f"{name}: {'PASS' if passed else 'FAIL'} exit={status} timeout={timed_out} "
              f"missing={len(missing)} forbidden={len(forbidden)}", flush=True)
    evidence["overall_pass"] = all_pass
    (out / "manifest.json").write_text(json.dumps(evidence, indent=2) + "\n")
    os.close(qemu_fd)
    os.close(activity_fd)
    return 0 if all_pass else 1


if __name__ == "__main__":
    raise SystemExit(main())

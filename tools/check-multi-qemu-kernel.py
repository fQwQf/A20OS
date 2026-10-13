#!/usr/bin/env python3
"""Run the real SMP task-migration test on stock x86 QEMU/KVM.

shared: two independent QEMU processes, one RAM file and kernel instance.
smp: ordinary single-QEMU SMP baseline. No QEMU sources are needed.
This is a kernel execution test, not a cross-instance Minecraft acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import socket
import subprocess
import tempfile
import time

ROOT = Path(__file__).resolve().parents[1]
FLAGS = ["ARCH=x86_64", "ABI=both", "BRINGUP=1", "NR_CPUS=2", "SMP_SELFTEST=1"]
PASS = "[SMP-TEST] PASS cpus=2 mask=0x3"


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def qmp_snapshot(path, process, deadline):
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(5)
        while True:
            try:
                sock.connect(str(path))
                break
            except (FileNotFoundError, ConnectionRefusedError):
                if process.poll() is not None or time.monotonic() >= deadline:
                    raise RuntimeError("QEMU exited or timed out opening QMP")
                time.sleep(.05)
        with sock.makefile("rwb", buffering=0) as stream:
            if "QMP" not in json.loads(stream.readline()):
                raise RuntimeError("invalid QMP greeting")

            def command(name):
                stream.write((json.dumps({"execute": name, "id": name}) + "\n").encode())
                while True:
                    line = stream.readline()
                    if not line:
                        raise RuntimeError("QMP closed")
                    reply = json.loads(line)
                    if reply.get("id") == name:
                        if "error" in reply:
                            raise RuntimeError(str(reply["error"]))
                        return reply["return"]

            command("qmp_capabilities")
            return {name: command(name) for name in
                    ("query-cpus-fast", "query-memdev", "query-kvm", "query-status")}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="new evidence directory")
    parser.add_argument("--mode", choices=("shared", "smp"), default="shared")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--timeout", type=float, default=90)
    args = parser.parse_args()
    if args.timeout <= 0 or args.jobs < 1:
        parser.error("timeout and jobs must be positive")
    qemu = shutil.which(args.qemu)
    if not qemu:
        parser.error("qemu-system-x86_64 is required")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        parser.error("accessible /dev/kvm required; cross-process TCG atomics are not validated")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)  # Never replace an earlier run.
    shared = args.mode == "shared"
    flags = FLAGS + [f"QEMU_SHARED_SMP={int(shared)}"]
    report = {"passed": False, "mode": args.mode, "make_flags": flags,
              "evidence": "kernel task migration; not userspace or Minecraft acceptance",
              "qemu": str(Path(qemu).resolve()), "qemu_sha256": sha(Path(qemu)),
              "qemu_version": subprocess.check_output([qemu, "--version"], text=True).splitlines()[0]}
    processes, logs = [], []
    try:
        report["git_head"] = subprocess.check_output(
            ["git", "rev-parse", "HEAD"], cwd=ROOT, text=True).strip()
        (out / "source.diff").write_bytes(subprocess.check_output(["git", "diff", "HEAD"], cwd=ROOT))
        report["source_sha256"] = {p: sha(ROOT / p) for p in (
            "Makefile", "tools/check-multi-qemu-kernel.py", "tools/boot/shared-qemu-reset.S",
            "kernel/arch/x86_64/boot/ap_trampoline.S", "kernel/arch/x86_64/platform/smp.c",
            "kernel/arch/x86_64/platform/timer.c", "kernel/arch/x86_64/trap/irqchip.c",
            "kernel/arch/x86_64/include/platform.h", "kernel/platform/qemu-virt-x86_64/board.c",
            "kernel/core/smp_selftest.c")}
        build = subprocess.check_output(["make", "-s", *flags, "print-build-dir"],
                                        cwd=ROOT, text=True).strip()
        with (out / "build.log").open("wb") as log:
            subprocess.run(["make", "--no-print-directory", *flags, "CCACHE=",
                            f"-j{args.jobs}", build + "/kernel.elf"],
                           cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
        # Freeze small boot artifacts before launching; later builds cannot change inputs.
        elf = out / "kernel.elf"
        shutil.copyfile(ROOT / build / "kernel.elf", elf)
        report["kernel_sha256"] = sha(elf)
        rom = out / "reset.bin"
        if shared:
            subprocess.run(["as", "--32", str(ROOT / "tools/boot/shared-qemu-reset.S"),
                            "-o", str(out / "reset.o")], check=True)
            subprocess.run(["ld", "-melf_i386", "-Ttext", "0", "--oformat", "binary",
                            "-o", str(rom), str(out / "reset.o")], check=True)
            if rom.stat().st_size != 65536:
                raise RuntimeError("reset ROM must be 64 KiB")
            report["reset_sha256"] = sha(rom)
        with tempfile.TemporaryDirectory(prefix="a20-shared-qemu-") as temp:
            runtime = Path(temp)
            ram = runtime / "ram"
            if shared:
                if shutil.disk_usage(runtime).free < 600 * 1024 * 1024:
                    raise RuntimeError("need at least 600 MiB free in temporary directory")
                with ram.open("xb") as f:
                    f.truncate(512 * 1024 * 1024)
                report["ram"] = {"path": str(ram), "inode": ram.stat().st_ino,
                                 "size": ram.stat().st_size}
            base = [qemu, "-machine", "q35" + (",memory-backend=shared" if shared else ""),
                    "-m", "512M", "-accel", "kvm", "-cpu", "host", "-smp", "1" if shared else "2",
                    "-display", "none", "-monitor", "none", "-nodefaults", "-serial", "stdio", "-no-reboot"]
            if shared:
                base += ["-object", f"memory-backend-file,id=shared,size=512M,mem-path={ram},share=on"]
            commands = [base + ["-kernel", str(elf)]]
            if shared:
                commands.append(base + ["-bios", str(rom)])
            for i, cmd in enumerate(commands):
                cmd += ["-qmp", f"unix:{runtime / str(i)},server=on,wait=off"]
            report["commands"] = commands
            processes = [None] * len(commands)
            try:
                deadline = time.monotonic() + args.timeout
                for i in reversed(range(len(commands))):
                    log = (out / f"node-{i}.log").open("wb")
                    logs.append(log)
                    processes[i] = subprocess.Popen(commands[i], cwd=ROOT, stdin=subprocess.DEVNULL,
                                                    stdout=log, stderr=subprocess.STDOUT)
                report["pids"] = [p.pid for p in processes]
                report["qmp"] = [qmp_snapshot(runtime / str(i), p, deadline)
                                 for i, p in enumerate(processes)]
                for snap in report["qmp"]:
                    if len(snap["query-cpus-fast"]) != (1 if shared else 2):
                        raise RuntimeError("wrong vCPU count")
                    if not snap["query-kvm"]["enabled"]:
                        raise RuntimeError("KVM not enabled")
                if shared:
                    # Capture kernel-observed file mappings, independently of command-line assertions.
                    maps = [Path(f"/proc/{p.pid}/maps").read_text() for p in processes]
                    report["ram_mappings"] = [[line for line in m.splitlines() if str(ram) in line]
                                              for m in maps]
                    for entries in report["ram_mappings"]:
                        if not entries or not all(line.split()[1] == "rw-s" and
                                int(line.split()[4]) == ram.stat().st_ino for line in entries):
                            raise RuntimeError("QEMU does not map the same RAM shared/writable")
                print(f"{args.mode}: running QEMU PIDs {report['pids']}", flush=True)
                while time.monotonic() < deadline:
                    texts = [(out / f"node-{i}.log").read_text(errors="replace")
                             for i in range(len(commands))]
                    if any("PANIC" in t or "panic:" in t for t in texts):
                        raise RuntimeError("guest panic")
                    if "[SMP-TEST] CLEANUP PASS" in texts[0]:
                        for marker in (PASS, "migrated=1->0", "heap=cross-cpu",
                                       "timer-irqs=all-online ipi=acked exit=0"):
                            if marker not in texts[0]:
                                raise RuntimeError("missing guest assertion: " + marker)
                        report["passed"] = True
                        break
                    if any(p.poll() is not None for p in processes):
                        raise RuntimeError("QEMU exited before guest acceptance")
                    time.sleep(.05)
                else:
                    raise RuntimeError("guest acceptance timeout")
            finally:
                # Stop only children owned by this run, before unlinking their RAM.
                for p in processes:
                    if p and p.poll() is None:
                        p.terminate()
                for p in processes:
                    if p:
                        try:
                            p.wait(timeout=5)
                        except subprocess.TimeoutExpired:
                            p.kill()
                            p.wait()
                report["returncodes"] = [p.returncode if p else None for p in processes]
                report["termination"] = "owned QEMU processes stopped by harness after acceptance/failure"
                report["temporary_ram_removed_after_stop"] = True
    except Exception as exc:
        report["failure"] = repr(exc)
        raise
    finally:
        for log in logs:
            log.close()
        report["log_sha256"] = {p.name: sha(p) for p in out.glob("node-*.log")}
        (out / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"PASS {args.mode}: same PID, stack and heap migrated CPU 1 -> 0; {out}")


if __name__ == "__main__":
    main()

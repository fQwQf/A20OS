#!/usr/bin/env python3
"""Run kernel migration or Native user-thread contracts on stock x86 QEMU/KVM.

shared: two independent QEMU processes, one RAM file and kernel instance.
smp: ordinary single-QEMU SMP baseline. No QEMU sources are needed.
Native mode uses the existing application, SDK and RAMFS; no disk emulation
is shared. Neither mode is cross-instance Minecraft acceptance.
"""
import argparse
import hashlib
import json
import os
import re
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
    parser.add_argument("--workload", choices=("kernel", "native"), default="kernel")
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
    if args.workload == "native":
        flags += ["SMP_SELFTEST=0", "RAMFS_USER=1", "RAMFS_NATIVE_TESTS=1"]
    report = {"passed": False, "mode": args.mode, "workload": args.workload, "make_flags": flags,
              "evidence": args.workload + " execution; not Minecraft acceptance",
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
            "kernel/core/smp_selftest.c", "kernel/fs/rootfs_user.c", "tools/targets-images.mk")}
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
                    processes[i] = subprocess.Popen(commands[i], cwd=ROOT, stdin=subprocess.PIPE if i == 0 and args.workload == "native" else subprocess.DEVNULL,
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
                application_started = False
                while time.monotonic() < deadline:
                    texts = [(out / f"node-{i}.log").read_text(errors="replace")
                             for i in range(len(commands))]
                    if any("PANIC" in t or "panic:" in t for t in texts):
                        raise RuntimeError("guest panic")
                    if args.workload == "native":
                        content = "\n".join(texts)
                        if "NATIVE_FUTEX: FAIL" in content:
                            raise RuntimeError("Native application contract failed")
                        if not application_started and re.search(r"(?:^|[\r\n])# ", content):
                            processes[0].stdin.write(b"/bin/native-futex; echo A20_NATIVE_EXIT:$?\n")
                            processes[0].stdin.flush()
                            application_started = True
                            deadline = time.monotonic() + args.timeout
                        if application_started and "NATIVE_FUTEX: PASS" in content and "A20_NATIVE_EXIT:0" in content:
                            markers = ["NATIVE_DOMAIN_CPUS: 02", "NATIVE_DOMAIN: PASS",
                                "NATIVE_COMPUTE_CANCEL: PASS workers=2 exit=137 pure-user=1",
                                "NATIVE_TASK_CONTROL: PASS signal=12 masked=1 exit=137",
                                "NATIVE_SPAWN: PASS children=2 exit=37 args=4 env=2 work=32",
                                "NATIVE_FILES_REOPEN: PASS count=4100",
                                "NATIVE_FILES: PASS bytes=2048 fnv=60f4edc5",
                                "NATIVE_ATOMIC32: PASS", "NATIVE_ALLOCATOR: PASS", "NATIVE_THREAD: PASS",
                                "NATIVE_RUNTIME: PASS", "NATIVE_OBJECT_DIRECTORY: PASS",
                                "NATIVE_BALANCE: PASS jobs=32 sum=4359168 mask=00000003 range=dynamic"]
                            if not all(marker in content for marker in markers):
                                raise RuntimeError("missing Native contract markers: " + str(
                                    [marker for marker in markers if marker not in content]))
                            cpus = re.search(r"NATIVE_THREAD_CPUS: (\d+)->(\d+)", content)
                            if not cpus or set(map(int, cpus.groups())) != {0, 1}:
                                raise RuntimeError("Native worker did not execute on remote CPU")
                            report.update(passed=True, native_thread_cpus=list(map(int, cpus.groups())),
                                          work_jobs=32, work_sum=4359168, work_cpu_mask=3, exit_code=0)
                            break
                    elif "[SMP-TEST] CLEANUP PASS" in texts[0]:
                        for marker in (PASS, "migrated=1->0", "heap=cross-cpu",
                                       "timer-irqs=all-online ipi=acked exit=0",
                                       "clock=" + ("ordered" if shared else "sampled") + " sleep=remote"):
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
                for p in processes:
                    if p and p.stdin:
                        p.stdin.close()
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
    print(f"PASS {args.mode} {args.workload}: verified guest execution; {out}")


if __name__ == "__main__":
    main()

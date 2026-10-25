#!/usr/bin/env python3
"""Run kernel, Native/Linux threads and remote disk I/O on stock x86 QEMU/KVM.

shared: two independent QEMU processes, one RAM file and kernel instance.
smp: ordinary single-QEMU SMP baseline. No QEMU sources are needed.
Disk modes attach a fresh scratch disk only to the primary. The secondary
has neither a disk nor a UART. Minecraft mode checks lifecycle; menu/input
acceptance additionally needs inspection of retained framebuffer screenshots
and interaction records.
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
LINUX_WORKLOADS = ("linux", "linux-disk", "linux-fs", "jvm", "jvm-threads", "gpu", "gpu-input", "minecraft")
USER_WORKLOADS = ("native", *LINUX_WORKLOADS)


def sha(path):
    h = hashlib.sha256()
    with path.open("rb") as f:
        for block in iter(lambda: f.read(1024 * 1024), b""):
            h.update(block)
    return h.hexdigest()


def verify_fsck_output(content, filesystem):
    # e2fsck -fn can return zero even after saying a mismatch was IGNORED.
    # Accept only the known clean five-pass transcript, not just exit status.
    seen = set()
    summary = False
    for line in content.splitlines():
        if not line or line.startswith("e2fsck "):
            continue
        stage = re.fullmatch(r"Pass ([1-5]): Checking .+", line)
        if stage:
            seen.add(int(stage[1]))
            continue
        if re.fullmatch(re.escape(str(filesystem)) + r": \d+/\d+ files \([^)]*\), \d+/\d+ blocks", line):
            summary = True
            continue
        raise RuntimeError("filesystem consistency diagnostic: " + line)
    if seen != {1, 2, 3, 4, 5} or not summary:
        raise RuntimeError("incomplete filesystem consistency check")



def minecraft_thread_evidence(content):
    """Summarize real guest procfs observations; this is not a per-CPU profiler."""
    roots, threads, cpus, samples = set(), {}, set(), 0
    pattern = r"MINECRAFT_THREAD_STAT: root=(\d+) (\d+) \(([^\n]*)\) ([^\n]+)"
    for match in re.finditer(pattern, content):
        fields = match[4].split()
        # comm can contain spaces. Fields after its closing ')' begin at 3.
        # Skip interrupted serial lines rather than interpreting mixed output.
        if len(fields) != 37: continue
        try:
            cpu = int(fields[36])
            ticks = int(fields[11]) + int(fields[12])
        except ValueError:
            continue
        if cpu not in (0, 1): raise RuntimeError("invalid guest thread CPU")
        root, tid = int(match[1]), int(match[2])
        roots.add(root)
        samples += 1
        entry = threads.setdefault(tid, {"name": match[3], "cpu_assignments": [], "max_ticks": 0})
        if cpu not in entry["cpu_assignments"]: entry["cpu_assignments"].append(cpu)
        entry["max_ticks"] = max(entry["max_ticks"], ticks)
        if ticks: cpus.add(cpu)
    if len(roots) != 1 or cpus != {0, 1} or len(threads) < 2:
        raise RuntimeError("missing same-JVM cross-CPU thread observations")
    return {"root": roots.pop(), "samples": samples, "cpu_mask": 3,
            "scope": "guest scheduler CPU assignments and accumulated ticks; not per-CPU runtime shares",
            "threads": threads}


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


def qmp_input(path):
    events = [
        {"type": "key", "data": {"down": True, "key": {"type": "qcode", "data": "a"}}},
        {"type": "key", "data": {"down": False, "key": {"type": "qcode", "data": "a"}}},
        {"type": "btn", "data": {"down": True, "button": "left"}},
        {"type": "btn", "data": {"down": False, "button": "left"}},
        {"type": "rel", "data": {"axis": "x", "value": 17}},
        {"type": "rel", "data": {"axis": "y", "value": -9}},
    ]
    with socket.socket(socket.AF_UNIX) as sock:
        sock.settimeout(10)
        sock.connect(str(path))
        with sock.makefile("rwb", buffering=0) as stream:
            if "QMP" not in json.loads(stream.readline()): raise RuntimeError("QMP greeting")
            for command in ({"execute": "qmp_capabilities"}, {"execute": "input-send-event", "arguments": {"events": events}}):
                stream.write((json.dumps(command) + "\n").encode())
                while True:
                    response = json.loads(stream.readline())
                    if "error" in response: raise RuntimeError(str(response["error"]))
                    if "return" in response: break
    return events


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--out", type=Path, required=True, help="new evidence directory")
    parser.add_argument("--workload", choices=("kernel", "native", "linux", "disk", "linux-disk", "linux-fs", "jvm", "jvm-threads", "gpu", "gpu-input", "minecraft"), default="kernel")
    parser.add_argument("--system-image", type=Path, help="existing raw root image for JVM; opened only as qcow2 backing")
    parser.add_argument("--ext4-checksums", choices=("legacy", "metadata32", "metadata64"), default="legacy")
    parser.add_argument("--mode", choices=("shared", "smp"), default="shared")
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--jobs", type=int, default=4)
    parser.add_argument("--memory-mib", type=int, help="RAM size; default 3072 for Minecraft, otherwise 512")
    parser.add_argument("--rendernode", default="/dev/dri/renderD128")
    parser.add_argument("--timeout", type=float, help="guest timeout; default 900 for Minecraft, otherwise 90")
    args = parser.parse_args()
    if args.timeout is None: args.timeout = 900 if args.workload == "minecraft" else 90
    memory_mib = args.memory_mib if args.memory_mib is not None else (3072 if args.workload == "minecraft" else 512)
    if memory_mib < 512 or memory_mib > 3072:
        parser.error("this runner currently supports 512..3072 MiB")
    if args.timeout <= 0 or args.jobs < 1:
        parser.error("timeout and jobs must be positive")
    qemu = shutil.which(args.qemu)
    if not qemu:
        parser.error("qemu-system-x86_64 is required")
    if not os.access("/dev/kvm", os.R_OK | os.W_OK):
        parser.error("accessible /dev/kvm required; cross-process TCG atomics are not validated")
    if args.workload in ("jvm", "jvm-threads", "minecraft") and (not args.system_image or not args.system_image.is_file()):
        parser.error("JVM requires an existing raw --system-image")
    out = args.out.resolve()
    out.mkdir(parents=True, exist_ok=False)  # Never replace an earlier run.
    shared = args.mode == "shared"
    flags = FLAGS + [f"QEMU_SHARED_SMP={int(shared)}"]
    if args.workload in USER_WORKLOADS:
        flags += ["SMP_SELFTEST=0", "RAMFS_USER=1", "RAMFS_NATIVE_TESTS=1"]
    if args.workload in ("gpu", "gpu-input", "minecraft"):
        flags += ["DRIVER_DEPLOYMENT=embedded"]
    report = {"passed": False, "mode": args.mode, "workload": args.workload, "make_flags": flags,
              "memory_mib": memory_mib,
              "evidence": args.workload + " execution; Minecraft visual/input acceptance needs retained screenshots and QMP events",
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
            "kernel/arch/x86_64/include/platform.h", "kernel/arch/x86_64/boards/qemu-virt-x86_64/board.c",
            "kernel/core/smp_selftest.c", "kernel/fs/rootfs_user.c", "tools/targets-images.mk",
            "kernel/proc/sched.c", "kernel/proc/current.c", "kernel/include/proc/proc.h",
            "user/cmds/stress/cluster_threads.c", "kernel/core/shared_device_call.c",
            "kernel/include/core/shared_device_call.h", "kernel/arch/x86_64/platform/shared_console.c",
            "kernel/arch/x86_64/include/console.h", "kernel/arch/x86_64/include/cpu.h",
            "kernel/core/shared_mmio.c", "kernel/include/core/shared_mmio.h",
            "kernel/drivers/core/driver_hwapi.h", "kernel/drivers/bus/pci_bus.c",
            "tools/driver-modules.mk", "kernel/drvmod/framework.c", "user/Makefile",
            "kernel/fs/mount_setup.c", "kernel/include/fs/mount_setup.h", "tools/mkrootfs.py",
            "kernel/fs/diskfs/ext4.c", "kernel/fs/diskfs/ext4_csum.c",
            "kernel/fs/procfs/procfs_render.c")}
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
                if shutil.disk_usage(runtime).free < (memory_mib + 128) * 1024 * 1024:
                    raise RuntimeError("not enough free space for shared RAM plus 128 MiB margin")
                with ram.open("xb") as f:
                    f.truncate(memory_mib * 1024 * 1024)
                report["ram"] = {"path": str(ram), "inode": ram.stat().st_ino,
                                 "size": ram.stat().st_size}
            base = [qemu, "-machine", "q35" + (",memory-backend=shared" if shared else ""),
                    "-m", f"{memory_mib}M", "-accel", "kvm", "-cpu", "host", "-smp", "1" if shared else "2",
                    "-display", "none", "-monitor", "none", "-nodefaults", "-serial", "stdio", "-no-reboot"]
            if shared:
                base += ["-object", f"memory-backend-file,id=shared,size={memory_mib}M,mem-path={ram},share=on"]
            commands = [base + ["-kernel", str(elf)]]
            if shared:
                secondary = list(base)
                serial = secondary.index("-serial")
                secondary[serial + 1] = "none"
                commands.append(secondary + ["-bios", str(rom)])
            disk = out / "scratch-disk.raw"
            if args.workload in ("disk", "linux-disk"):
                with disk.open("xb") as f:
                    f.truncate(2 * 1024 * 1024)
                    f.seek(7 * 512)
                    f.write(bytes((i * 17 + 0xa2) & 255 for i in range(512)))
                report["disk_before_sha256"] = sha(disk)
                commands[0] += ["-drive", f"file={disk},format=raw,if=none,id=scratch,cache=writeback",
                                "-device", "virtio-blk-pci,drive=scratch,disable-legacy=on"]
                if args.workload == "disk":
                    commands[0] += ["-fw_cfg", "name=opt/x86/cmdline,string=a20.smp_disk_test=1"]
            filesystem = out / "scratch-ext4.img"
            if args.workload == "linux-fs":
                seed = out / "seed"
                seed.mkdir()
                user_variant = "x86_64" + ("-shared-qemu" if shared else "")
                shutil.copyfile(ROOT / "user/build" / user_variant / "cluster_threads", seed / "program")
                (seed / "program").chmod(0o755)
                with filesystem.open("xb") as f: f.truncate(32 * 1024 * 1024)
                from mkrootfs import EXT4_MKFS_ARGS, resolve_mkfs_ext4
                mkfs_args = list(EXT4_MKFS_ARGS[1:])
                if args.ext4_checksums != "legacy":
                    mkfs_args[mkfs_args.index("-O") + 1] += ",^uninit_bg,metadata_csum," + ("64bit" if args.ext4_checksums == "metadata64" else "^64bit")
                mkfs = [resolve_mkfs_ext4(), *mkfs_args, "-d", str(seed), str(filesystem)]
                with (out / "mkfs.log").open("wb") as log:
                    subprocess.run(mkfs, stdout=log, stderr=subprocess.STDOUT, check=True)
                report.update(filesystem_before_sha256=sha(filesystem), disk_program_sha256=sha(seed / "program"), mkfs=mkfs, ext4_checksums=args.ext4_checksums)
                commands[0] += ["-drive", f"file={filesystem},format=raw,if=none,id=filesystem,cache=writeback",
                                "-device", "virtio-blk-pci,drive=filesystem,disable-legacy=on"]
            if args.workload in ("jvm", "jvm-threads", "minecraft"):
                source = args.system_image.resolve()
                report.update(system_image=str(source), system_image_before_sha256=sha(source))
                overlay = out / "jvm-root.qcow2"
                subprocess.run(["qemu-img", "create", "-q", "-f", "qcow2", "-F", "raw",
                                "-b", str(source), str(overlay)], check=True)
                commands[0] += ["-drive", f"file={overlay},format=qcow2,if=none,id=jvm,cache=writeback",
                                "-device", "virtio-blk-pci,drive=jvm,disable-legacy=on"]
            if args.workload == "jvm-threads":
                seed = out / "jvm-payload"
                seed.mkdir()
                java = shutil.which("java") or "/usr/lib/jvm/java-21-openjdk-amd64/bin/java"
                compile_java = [java, "-m", "jdk.compiler/com.sun.tools.javac.Main", "-source", "21", "-target", "21",
                                "-d", str(seed), str(ROOT / "tools/shared-jvm-test/ClusterProbe.java")]
                compile_jni = ["x86_64-linux-gnu-gcc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                               "-fPIC", "-shared", "-nostdlib", "-Wl,--hash-style=both",
                               str(ROOT / "tools/shared-jvm-test/cpu.c"), "-o", str(seed / "libclusterprobe.so")]
                with (out / "payload-build.log").open("wb") as log:
                    subprocess.run(compile_java, stdout=log, stderr=subprocess.STDOUT, check=True)
                    subprocess.run(compile_jni, stdout=log, stderr=subprocess.STDOUT, check=True)
                payload = out / "jvm-payload.img"
                with payload.open("xb") as f: f.truncate(16 * 1024 * 1024)
                from mkrootfs import EXT4_MKFS_ARGS, resolve_mkfs_ext4
                subprocess.run([resolve_mkfs_ext4(), *EXT4_MKFS_ARGS[1:], "-d", str(seed), str(payload)], check=True)
                commands[0] += ["-drive", f"file={payload},format=raw,if=none,id=payload,readonly=on",
                                "-device", "virtio-blk-pci,drive=payload,disable-legacy=on"]
                report.update(java_compiler=compile_java, jni_compiler=compile_jni,
                              java_payload_sha256=sha(payload),
                              java_sources={str(p.relative_to(ROOT)): sha(p) for p in
                                            (ROOT / "tools/shared-jvm-test").iterdir() if p.is_file()})
            if args.workload in ("gpu", "gpu-input"):
                seed = out / "gpu-payload"
                seed.mkdir()
                variant = "x86_64" + ("-shared-qemu" if shared else "")
                shutil.copyfile(ROOT / "user/build" / variant / "gpu3d_test", seed / "program")
                (seed / "program").chmod(0o755)
                gpu_payload = out / "gpu-payload.img"
                with gpu_payload.open("xb") as f: f.truncate(16 * 1024 * 1024)
                from mkrootfs import EXT4_MKFS_ARGS, resolve_mkfs_ext4
                subprocess.run([resolve_mkfs_ext4(), *EXT4_MKFS_ARGS[1:], "-d", str(seed), str(gpu_payload)], check=True)
                commands[0][commands[0].index("-display") + 1] = f"egl-headless,rendernode={args.rendernode}"
                commands[0] += ["-vga", "none", "-device", "virtio-gpu-gl-pci",
                                "-drive", f"file={gpu_payload},format=raw,if=none,id=gpu_payload,readonly=on",
                                "-device", "virtio-blk-pci,drive=gpu_payload,disable-legacy=on"]
                report.update(gpu_payload_sha256=sha(gpu_payload), gpu_program_sha256=sha(seed / "program"),
                              gpu_test_source_sha256=sha(ROOT / "user/cmds/core/gpu3d_test.c"))
            if args.workload in ("gpu-input", "minecraft"):
                commands[0] += ["-device", "virtio-keyboard-pci", "-device", "virtio-mouse-pci"]
            if args.workload == "minecraft":
                seed = out / "minecraft-payload"
                shutil.copytree(ROOT / "tools/shared-minecraft-test", seed)
                variant = "x86_64" + ("-shared-qemu" if shared else "")
                binaries = [f"user/build/{variant}/{n}-x86_64" for n in
                            ("cluster-supervisor", "appd", "app-exec", "appctl-run")]
                with (out / "native-build.log").open("wb") as log:
                    subprocess.run(["make", "--no-print-directory", *flags, "CCACHE=", f"-j{args.jobs}", *binaries],
                                   cwd=ROOT, stdout=log, stderr=subprocess.STDOUT, check=True)
                for binary in binaries:
                    shutil.copyfile(ROOT / binary, seed / Path(binary).name)
                for name, source_path in (("minecraft", "packages/overlay/xfce/usr/local/bin/minecraft"),
                                          ("java", "packages/overlay/xfce/usr/bin/java")):
                    shutil.copyfile(ROOT / source_path, seed / name)
                for p in seed.iterdir(): p.chmod(0o755)
                payload = out / "minecraft-payload.img"
                with payload.open("xb") as f: f.truncate(16 * 1024 * 1024)
                from mkrootfs import EXT4_MKFS_ARGS, resolve_mkfs_ext4
                subprocess.run([resolve_mkfs_ext4(), *EXT4_MKFS_ARGS[1:], "-d", str(seed), str(payload)], check=True)
                commands[0][commands[0].index("-display") + 1] = f"egl-headless,rendernode={args.rendernode}"
                commands[0] += ["-vnc", f"unix:{runtime / 'vnc'}",
                                "-vga", "none", "-device", "virtio-gpu-gl-pci",
                                "-audiodev", "driver=none,id=a20audio", "-device", "intel-hda",
                                "-device", "hda-duplex,audiodev=a20audio",
                                "-drive", f"file={payload},format=raw,if=none,id=payload,readonly=on",
                                "-device", "virtio-blk-pci,drive=payload,disable-legacy=on"]
                report.update(minecraft_payload_sha256=sha(payload), minecraft_files={p.name: sha(p) for p in seed.iterdir()})
            for i, cmd in enumerate(commands):
                cmd += ["-qmp", f"unix:{runtime / str(i)},server=on,wait=off"]
            report["commands"] = commands
            processes = [None] * len(commands)
            try:
                deadline = time.monotonic() + args.timeout
                for i in reversed(range(len(commands))):
                    log = (out / f"node-{i}.log").open("wb")
                    logs.append(log)
                    processes[i] = subprocess.Popen(commands[i], cwd=ROOT, stdin=subprocess.PIPE if i == 0 and args.workload in USER_WORKLOADS else subprocess.DEVNULL,
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
                print(f"{args.mode}: running QEMU PIDs {report['pids']}; primary QMP {runtime / '0'}", flush=True)
                (out / "running.json").write_text(json.dumps(report, indent=2) + "\n")
                application_started = False
                console_input_sent = False
                hardware_input_sent = False
                while time.monotonic() < deadline:
                    texts = [(out / f"node-{i}.log").read_text(errors="replace")
                             for i in range(len(commands))]
                    if any("PANIC" in t or "panic:" in t for t in texts):
                        raise RuntimeError("guest panic")
                    if args.workload in USER_WORKLOADS:
                        content = texts[0]
                        if shared and ("NATIVE_" in texts[1] or "CLUSTER_THREADS:" in texts[1]):
                            raise RuntimeError("user output escaped primary console owner")
                        if "NATIVE_FUTEX: FAIL" in content or "CLUSTER_THREADS: FAIL" in content:
                            raise RuntimeError("user application contract failed")
                        if not application_started and re.search(r"(?:^|[\r\n])# ", content):
                            processes[0].stdin.write((b"/bin/native-futex; echo A20_NATIVE_EXIT:$?\n" if args.workload == "native"
                                                      else ("/bin/cluster_threads --console" + (" --disk" if args.workload == "linux-disk" else " --filesystem" if args.workload == "linux-fs" else " --jvm" if args.workload == "jvm" else " --jvm-threads" if args.workload == "jvm-threads" else " --gpu" if args.workload == "gpu" else " --gpu-input" if args.workload == "gpu-input" else " --minecraft" if args.workload == "minecraft" else "") + "; echo A20_LINUX_EXIT:$?\n").encode()))
                            processes[0].stdin.flush()
                            application_started = True
                            deadline = time.monotonic() + args.timeout
                        if args.workload in LINUX_WORKLOADS and not console_input_sent and "CLUSTER_CONSOLE: remote-cpu=1 ready" in content:
                            processes[0].stdin.write(b"owner-ok\n")
                            processes[0].stdin.flush()
                            console_input_sent = True
                        if args.workload == "gpu-input" and not hardware_input_sent and "CLUSTER_INPUT: ready cpu=1" in content:
                            report["input_events"] = qmp_input(runtime / "0")
                            hardware_input_sent = True
                        exited = re.search(r"A20_(?:NATIVE|LINUX)_EXIT:(\d+)", content)
                        if application_started and exited and int(exited[1]) != 0:
                            raise RuntimeError("user application exited " + exited[1])
                        if args.workload in LINUX_WORKLOADS and "CLUSTER_THREADS: PASS" in content and "A20_LINUX_EXIT:0" in content:
                            match = re.search(r"CLUSTER_THREADS: PASS threads=2 affinity-rounds=128 migrations=(\d+) tls=preserved tlb-rounds=64 protection-faults=128 jobs=1024 split=(\d+)/(\d+) sum=(\d+) mask=3 destructors=2 cond-timeout=checked", content)
                            if not match or not 126 <= int(match[1]) <= 128 or sum(map(int, match.groups()[1:3])) != 1024 or min(map(int, match.groups()[1:3])) <= 0 or int(match[4]) != 8256 * 1024 * 1025 // 2:
                                raise RuntimeError("missing Linux/POSIX execution evidence")
                            if not console_input_sent or "CLUSTER_CONSOLE: PASS cpu=1 input=owner-ok blocks=32" not in content:
                                raise RuntimeError("missing remote console input/output acceptance")
                            if args.workload == "linux-disk" and "CLUSTER_DISK: PASS cpu=1 sectors=32 bytes=16384 flush=32 readback=32" not in content:
                                raise RuntimeError("missing user disk execution")
                            if args.workload == "linux-fs":
                                for marker in ("CLUSTER_FS_CHILD: PASS cpu=1 bytes=65536 fsync=file+directory",
                                               "CLUSTER_FS: PASS cpu=1 exec=disk child-exit=37 bytes=65536 unmounted=1"):
                                    if marker not in content: raise RuntimeError("missing filesystem acceptance: " + marker)
                            if args.workload == "jvm":
                                if 'openjdk version "21.' not in content or "CLUSTER_JVM: PASS cpu=1 java-exit=0 unmounted=1" not in content:
                                    raise RuntimeError("missing real JVM acceptance")
                                report.update(jvm_version_started=True, jvm_cpu=1, jvm_exit=0)
                            if args.workload == "jvm-threads":
                                java_result = re.search(r"CLUSTER_JAVA: PASS threads=2 affinity-checks=16 mask=3 jobs=512 split=(\d+)/(\d+) sum=(\d+)", content)
                                if (not java_result or sum(map(int, java_result.groups()[:2])) != 512 or
                                    min(map(int, java_result.groups()[:2])) < 1 or int(java_result[3]) != 8256 * 512 * 513 // 2 or
                                    "CLUSTER_JVM: PASS affinity-mask=3 java-exit=0 unmounted=1" not in content):
                                    raise RuntimeError("missing cross-CPU Java work execution")
                                report.update(java_threads=2, java_affinity_checks=16, java_cpu_mask=3,
                                              java_work_split=list(map(int, java_result.groups()[:2])), java_sum=int(java_result[3]), jvm_exit=0)
                            if args.workload in ("gpu", "gpu-input"):
                                for marker in ("GPU3D_TEST: PASS (UAPI works and the host rendered the colours",
                                               "GPU3D_TEST: DMA-BUF CPU upload/download PASS",
                                               "GPU3D_TEST: native fence export/import/poll PASS",
                                               "GPU3D_TEST: native allocation 96 simultaneous objects PASS",
                                               "CLUSTER_GPU: PASS cpu=1 exit=0 unmounted=1"):
                                    if marker not in content: raise RuntimeError("missing remote GPU acceptance: " + marker)
                                if content.count("GPU3D_TEST: pixel readback ok") < 2:
                                    raise RuntimeError("missing rendered pixel readback")
                                report.update(gpu_cpu=1, gpu_exit=0, gpu_pixels_verified=True)
                                if args.workload == "gpu-input":
                                    if not hardware_input_sent or "CLUSTER_INPUT: PASS cpu=1 key=a press+release button=left press+release relative=17,-9" not in content:
                                        raise RuntimeError("missing remote keyboard/mouse acceptance")
                                    report.update(hardware_input_verified=True, input_cpu=1)
                            if args.workload == "minecraft":
                                if "MINECRAFT_OWNER: appctl exit status=0" not in content or "CLUSTER_MINECRAFT: session-exit=0" not in content:
                                    raise RuntimeError("missing Minecraft application/session exit")
                                for marker in ("CLUSTER_PROC_CPU: PASS field=39 cpus=0,1",
                                               "[Render thread/INFO]: Backend library: LWJGL version",
                                               "minecraft:textures/atlas/gui.png-atlas"):
                                    if marker not in content: raise RuntimeError("missing Minecraft runtime evidence: " + marker)
                                report.update(minecraft_exit=0, minecraft_threads=minecraft_thread_evidence(content),
                                              visual_acceptance="requires separate screenshot inspection")
                            for block in range(32):
                                marker = f"CLUSTER_CONSOLE_BLOCK:{block:02d}:abcdefghijklmnopqrstuvwxyz012345"
                                if content.count(marker) != 1:
                                    raise RuntimeError("missing/duplicate remote console block " + str(block))
                            report.update(passed=True, console_owner=0, console_client_cpu=1, console_blocks=32, linux_threads=2, migrations=int(match[1]), tlb_rounds=64, protection_faults=128,
                                          work_split=list(map(int, match.groups()[1:3])), work_sum=int(match[4]), exit_code=0)
                            break
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
                        if args.workload in ("disk", "linux-disk") and "[SMP-DISK] PASS cpu=1 sectors=32 bytes=16384 flush=32 readback=32" not in texts[0]:
                            raise RuntimeError("missing remote disk execution")
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
            if args.workload in ("disk", "linux-disk"):
                actual = disk.read_bytes()
                expected = bytearray(2 * 1024 * 1024)
                expected[7*512:8*512] = bytes((i * 17 + 0xa2) & 255 for i in range(512))
                for round in range(32):
                    expected[(8+round)*512:(9+round)*512] = bytes((i * 29 + round * 7 + 0x20) & 255 for i in range(512))
                if actual != expected:
                    report["passed"] = False
                    raise RuntimeError("host scratch disk content mismatch")
                report.update(disk_after_sha256=sha(disk), disk_host_verified=True,
                              disk_owner=0, disk_client_cpu=1, disk_bytes=16384,
                              disk_flushes=32)
            if args.workload == "linux-fs":
                debugfs = shutil.which("debugfs") or "/usr/sbin/debugfs"
                fsck = shutil.which("e2fsck") or "/usr/sbin/e2fsck"
                # All QEMUs have stopped. Read the actual filesystem via an
                # independent host implementation and check its consistency.
                with (out / "debugfs.log").open("wb") as log:
                    subprocess.run([debugfs, "-R", f"dump /result.bin {out / 'result.bin'}", str(filesystem)],
                                   stdout=log, stderr=subprocess.STDOUT, check=True)
                    subprocess.run([debugfs, "-R", f"dump /program {out / 'disk-program'}", str(filesystem)],
                                   stdout=log, stderr=subprocess.STDOUT, check=True)
                if sha(out / "disk-program") != report["disk_program_sha256"]:
                    raise RuntimeError("disk executable changed")
                expected = bytes((i * 13 + block * 7 + 0xa2) & 255 for block in range(16) for i in range(4096))
                if (out / "result.bin").read_bytes() != expected:
                    raise RuntimeError("host filesystem data mismatch")
                with (out / "fsck.log").open("wb") as log:
                    subprocess.run([fsck, "-fn", str(filesystem)], stdout=log, stderr=subprocess.STDOUT, check=True,
                                   env={**os.environ, "LC_ALL": "C"})
                fsck_text = (out / "fsck.log").read_text(errors="replace")
                verify_fsck_output(fsck_text, filesystem)
                report.update(filesystem_after_sha256=sha(filesystem), filesystem_host_verified=True,
                              filesystem_bytes=65536, filesystem_child_exit=37, filesystem_fsck=True)
            if args.workload in ("jvm", "jvm-threads", "minecraft"):
                report["system_image_after_sha256"] = sha(source)
                if report["system_image_after_sha256"] != report["system_image_before_sha256"]:
                    raise RuntimeError("backing system image changed during test")
                report["jvm_overlay_sha256"] = sha(overlay)
    except Exception as exc:
        report["passed"] = False
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

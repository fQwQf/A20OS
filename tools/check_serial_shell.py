#!/usr/bin/env python3
"""Validate diskless RAMFS shell commands on QEMU; this does not emulate a physical board."""
import argparse
import contextlib
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import tempfile
import time

from a20_resource import Policy, Requirement, gate
from check_arm32_shell import handoff_elf

ROOT = Path(__file__).resolve().parents[1]
MARKERS = ("A20_EXEC_ONE", "A20_EXEC_TWO", "A20_UNAME_OK", "A20_SLEEP_OK",
           "A20_SUBSHELL_OK", "A20_FILE_CONTENT", "A20_FILES_OK", "A20_SYNC_OK")


def commands(shutdown, fpu=False, stress_forks=0, timers=False):
    script = (
        "/bin/echo A20_EXEC_ONE", "/bin/echo A20_EXEC_TWO",
        "/bin/uname -s > /tmp/a20-uname && test -s /tmp/a20-uname && echo A20_UNAME_OK",
        "/bin/sleep 1 && echo A20_SLEEP_OK", "(echo A20_SUBSHELL_OK)",
        "/bin/mkdir /tmp/a20-shell && /bin/echo A20_FILE_CONTENT > /tmp/a20-shell/original "
        "&& /bin/cp /tmp/a20-shell/original /tmp/a20-shell/copy "
        "&& /bin/cat /tmp/a20-shell/copy "
        "&& /bin/rm /tmp/a20-shell/original /tmp/a20-shell/copy "
        "&& test ! -e /tmp/a20-shell/copy && echo A20_FILES_OK",
        "/bin/sync && echo A20_SYNC_OK", "/bin/" + shutdown,
    )
    # Split every marker with shell quoting on the serial wire. Even wrapped
    # command echoes cannot supply an intact marker to the output oracle.
    for marker in MARKERS:
        script = tuple(line.replace(marker, marker[:4] + "'" + marker[4:] + "'")
                       for line in script)
    probes = (("/bin/fork_fpu_probe",) if fpu else ())
    if timers:
        probes += ("/bin/timer_preempt", "/bin/timer_idle")
    if stress_forks:
        probes += (f'i=0; while test "$i" -lt {stress_forks}; do '
                   '/bin/echo child > /tmp/a20-child || break; i=$((i+1)); done; '
                   f'test "$i" -eq {stress_forks} && echo A20_\'STRESS_OK\'',)
    return probes + script


def validate_output(text, status, shutdown, fpu=False, stress_forks=0, timers=False):
    lines = text.replace("\r", "").splitlines()
    missing = [marker for marker in MARKERS if marker not in lines]
    if fpu and "FORK_FPU: PASS" not in lines:
        missing.append("FORK_FPU: PASS")
    if stress_forks and "A20_STRESS_OK" not in lines:
        missing.append("A20_STRESS_OK")
    if timers:
        missing += [marker for marker in ("TIMER_PREEMPT: PASS", "TIMER_IDLE: PASS") if marker not in lines]
    final = "Rebooting system." if shutdown == "reboot" else "System is going down for power-off NOW."
    errors = [word for word in ("SIGSEGV", "KERNEL PANIC", "UBSAN:", "can't fork", "FORK_FPU: FAIL") if word in text]
    if status or missing or final not in lines or errors:
        raise ValueError(f"exit={status}, missing={missing}, shutdown={final in lines}, errors={errors}")


@contextlib.contextmanager
def qemu_command(arch, kernel, memory):
    executable = "qemu-system-" + ("arm" if arch == "arm32" else arch)
    qemu = shutil.which(executable)
    if not qemu:
        raise ValueError(executable + " is required")
    command = [qemu, "-machine", "virt", "-m", str(memory) + "M", "-smp", "1",
               "-display", "none", "-serial", "stdio", "-monitor", "none", "-net", "none", "-no-reboot"]
    if arch == "arm32":
        with tempfile.TemporaryDirectory(prefix="a20-shell-dt-") as temporary:
            stage = Path(temporary)
            dtb = stage / "virt.dtb"
            subprocess.run([qemu, "-machine", "virt,dumpdtb=" + str(dtb), "-cpu", "cortex-a15",
                            "-m", str(memory) + "M", "-display", "none", "-net", "none"],
                           check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=10)
            physical = 0x40000000 + memory * 1024 * 1024 - 1024 * 1024
            shim = stage / "handoff.elf"
            shim.write_bytes(handoff_elf(physical))
            yield command + ["-cpu", "cortex-a15", "-kernel", str(shim), "-device",
                             "loader,file=" + str(kernel), "-device",
                             f"loader,file={dtb},addr={physical},force-raw=on"]
        return
    if arch == "aarch64":
        # QEMU supplies Linux's DT handoff for Image format, not bare ELF.
        image = kernel.with_suffix(".bin") if kernel.suffix == ".elf" else kernel
        if not image.is_file():
            raise ValueError("AArch64 needs the sibling kernel.bin Image")
        command += ["-cpu", "cortex-a57", "-kernel", str(image)]
    elif arch == "riscv64":
        command += ["-bios", "default", "-kernel", str(kernel)]
    else:
        command += ["-cpu", "max", "-kernel", str(kernel)]
    yield command


def run(command, timeout, shutdown, fpu=False, stress_forks=0, timers=False):
    output = bytearray()
    sent = False
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as proc:
        with selectors.DefaultSelector() as selector:
            selector.register(proc.stdout, selectors.EVENT_READ)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline and proc.poll() is None:
                for key, _ in selector.select(0.2):
                    chunk = os.read(key.fd, 65536)
                    if chunk:
                        output.extend(chunk)
                    else:
                        selector.unregister(key.fileobj)
                if not sent and b"# " in output:
                    proc.stdin.write(("\n".join(commands(shutdown, fpu, stress_forks, timers)) + "\n").encode())
                    proc.stdin.flush()
                    sent = True
            if proc.poll() is None:
                proc.kill()
            tail, _ = proc.communicate()
            output.extend(tail)
            return output, proc.returncode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--arch", choices=("arm32", "riscv64", "aarch64", "loongarch64"), required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--memory-mb", type=int, default=1024)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--shutdown", choices=("poweroff", "reboot"), default="poweroff")
    parser.add_argument("--fpu", action="store_true", help="require the optional RAMFS_FPU_TESTS probe")
    parser.add_argument("--stress-forks", type=int, default=0, help="sequential external-command fork/wait pressure")
    parser.add_argument("--timers", action="store_true", help="require CPU-hog preemption and idle timeout probes")
    parser.add_argument("--log", type=Path)
    args = parser.parse_args()
    if not args.kernel.is_file() or args.memory_mb < 64 or args.timeout <= 0 or args.stress_forks < 0:
        parser.error("a kernel, >=64 MiB RAM and positive timeout are required")
    policy = Policy.from_env()
    gate(Requirement(args.memory_mb + policy.reserve_mem_mb, 1, policy.min_disk_mb),
         policy, ROOT, wait=False, label=args.arch + " RAMFS shell")
    log = args.log or ROOT / ".kernel-build/smoke" / (args.arch + "-serial-shell.log")
    try:
        with qemu_command(args.arch, args.kernel.resolve(), args.memory_mb) as command:
            output, status = run(command, args.timeout, args.shutdown, args.fpu, args.stress_forks, args.timers)
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_bytes(output)
        validate_output(output.decode(errors="replace"), status, args.shutdown, args.fpu, args.stress_forks, args.timers)
    except (ValueError, subprocess.SubprocessError) as error:
        print(f"FAIL: {error}; log: {log}")
        return 1
    print(f"PASS: {args.arch} {args.memory_mb} MiB RAMFS external commands, timers, files, {args.shutdown}; log: {log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

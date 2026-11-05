#!/usr/bin/env python3
"""Exercise a diskless ARM32 RAMFS shell under QEMU, including child exit and timers."""
import argparse
import os
from pathlib import Path
import selectors
import shutil
import subprocess
import struct
import sys
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cli"))
from a20_resource import Policy, Requirement, gate
MARKERS = ("ARM32_EXTERNAL_ONE", "ARM32_EXTERNAL_TWO", "ARM32_SLEEP_DONE",
           "ARM32_SUBSHELL", "ARM32_FILE_CONTENT", "ARM32_SHELL_PASS")
COMMANDS = ("/bin/echo ARM32_EXTERNAL_ONE", "/bin/echo ARM32_EXTERNAL_TWO",
            "/bin/sleep 1; echo ARM32_SLEEP_DONE", "(echo ARM32_SUBSHELL)",
            "echo ARM32_FILE_CONTENT > /tmp/arm32-shell; /bin/cat /tmp/arm32-shell",
            "echo ARM32_SHELL_PASS", "/bin/poweroff")
# Keep the complete marker off the serial input wire, including line wraps.
# The shell removes these quotes; a terminal echo cannot satisfy the oracle.
for _marker in MARKERS:
    COMMANDS = tuple(command.replace(_marker, _marker[:4] + "'" + _marker[4:] + "'")
                     for command in COMMANDS)


def handoff_elf(dtb_address):
    """ARM Linux handoff: preserve a real QEMU DTB in r2, then enter the kernel.

    QEMU's direct ELF entry has no Linux boot stub and leaves r2 unset. A tiny
    test-only shim gives the same register contract as U-Boot bootm.
    """
    address = 0x40000000
    code = struct.pack("<5I", 0xe59f2004, 0xe59ff004, 0xe1a00000,
                       dtb_address, 0x40080000)
    ident = b"\x7fELF\x01\x01\x01" + bytes(9)
    header = struct.pack("<16sHHIIIIIHHHHHH", ident, 2, 40, 1, address,
                         52, 0, 0x5000400, 52, 32, 1, 0, 0, 0)
    segment = struct.pack("<8I", 1, 0x100, address, address, len(code),
                          len(code), 5, 4)
    return (header + segment).ljust(0x100, b"\0") + code


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--timeout", type=float, default=90)
    parser.add_argument("--memory-mb", type=int, default=1024,
                        help="guest RAM; 64 checks the V3s memory budget on QEMU virt")
    parser.add_argument("--log", type=Path, default=ROOT / ".kernel-build/smoke/arm32-shell.log")
    args = parser.parse_args()
    if not args.kernel.is_file() or args.timeout <= 0 or args.memory_mb < 64:
        parser.error("a kernel ELF, positive timeout and at least 64 MiB RAM are required")
    qemu = shutil.which("qemu-system-arm")
    if not qemu:
        parser.error("qemu-system-arm is required")
    policy = Policy.from_env()
    gate(Requirement(args.memory_mb + policy.reserve_mem_mb, 1, policy.min_disk_mb),
         policy, ROOT, wait=False, label="ARM32 shell smoke")
    command = [qemu, "-machine", "virt", "-cpu", "cortex-a15", "-m", str(args.memory_mb) + "M",
               "-smp", "1", "-display", "none", "-serial", "stdio", "-monitor", "none",
               "-net", "none", "-no-reboot"]
    temporary = tempfile.TemporaryDirectory(prefix="a20-arm32-handoff-")
    stage = Path(temporary.name)
    dtb = stage / "virt.dtb"
    subprocess.run([qemu, "-machine", "virt,dumpdtb=" + str(dtb), "-cpu", "cortex-a15",
                    "-m", str(args.memory_mb) + "M", "-display", "none", "-net", "none"],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                   timeout=10)
    dtb_address = 0x40000000 + args.memory_mb * 1024 * 1024 - 1024 * 1024
    shim = stage / "handoff.elf"
    shim.write_bytes(handoff_elf(dtb_address))
    command += ["-kernel", str(shim), "-device", "loader,file=" + str(args.kernel.resolve()),
                "-device", f"loader,file={dtb},addr={dtb_address},force-raw=on"]
    output = bytearray()
    sent = False
    args.log.parent.mkdir(parents=True, exist_ok=True)
    with subprocess.Popen(command, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                          stderr=subprocess.STDOUT) as proc:
        selector = selectors.DefaultSelector()
        selector.register(proc.stdout, selectors.EVENT_READ)
        deadline = time.monotonic() + args.timeout
        while time.monotonic() < deadline and proc.poll() is None:
            for key, _ in selector.select(0.2):
                chunk = os.read(key.fd, 65536)
                if not chunk:
                    selector.unregister(key.fileobj)
                    continue
                output.extend(chunk)
            # Prompt includes cwd and is emitted only after successful exec.
            if not sent and b"# " in output:
                proc.stdin.write(("\n".join(COMMANDS) + "\n").encode())
                proc.stdin.flush()
                sent = True
        if proc.poll() is None:
            proc.kill()
        tail, _ = proc.communicate()
        output.extend(tail)
        selector.close()
        status = proc.returncode
    temporary.cleanup()
    args.log.write_bytes(output)
    text = output.decode(errors="replace").replace("\r", "")
    # Match complete output lines, never terminal echoes of command input.
    lines = text.splitlines()
    missing = [marker for marker in MARKERS if marker not in lines]
    if status or missing or "System is going down for power-off NOW" not in text or \
            "SIGSEGV" in text or "KERNEL PANIC" in text or "UBSAN:" in text:
        print(f"FAIL: exit={status}, missing={missing}; log: {args.log}")
        return 1
    print(f"PASS: ARM32 external exec, wait, sleep, subshell, RAM file and poweroff; log: {args.log}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

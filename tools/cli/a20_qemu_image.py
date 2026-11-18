"""QEMU handoff for a supplied kernel image, shared by observe and tests."""
import contextlib
from pathlib import Path
import shutil
import subprocess
import tempfile
import struct

def handoff_elf(dtb_address):
    """ARM Linux handoff: preserve a real QEMU DTB in r2, then enter the kernel.

    QEMU's direct ELF entry has no Linux boot stub and leaves r2 unset. A tiny
    boot shim gives the same register contract as U-Boot bootm.
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



@contextlib.contextmanager
def qemu_command(arch, kernel, memory):
    executable = "qemu-system-" + ("arm" if arch == "arm32" else arch)
    qemu = shutil.which(executable)
    if not qemu:
        raise ValueError(executable + " is required")
    command = [qemu, "-machine", "pc" if arch == "x86_64" else "virt", "-m", str(memory) + "M", "-smp", "1",
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


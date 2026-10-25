#!/usr/bin/env python3
"""Exercise the production serial handoff against real U-Boot and an A20 kernel.

Requires an upstream qemu-riscv64_smode_defconfig U-Boot build and an already
built A20 qemu-riscv64 bring-up Image. Writes only a temporary regular SD image.
This validates the boot protocol, not a physical Nano/PINE/2K0300 board.
"""
import argparse
from dataclasses import replace
import os
from pathlib import Path
import pty
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from a20_console import SerialTransport, write_console_log
from a20_handoff import run_image_handoff
from a20_instance import parse_instance
from a20_make import query_make
from kernel_bundle import package
from pine_a64 import mbr, PARTITION_OFFSET


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uboot", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "build/hardware-validation/uboot-handoff-qemu")
    args = ap.parse_args()
    out = args.out.absolute()
    out.mkdir(parents=True, exist_ok=True)
    source = parse_instance(ROOT / "instances/smoke-riscv64.toml")
    vars_ = query_make(source, ("KERNEL_ELF", "KERNEL_BIN"))
    fixture = out / "qemu.toml"
    fixture.write_text('''name = "uboot-handoff-qemu"
arch = "riscv64"
board = "qemu-virt-riscv64"
[kernel]
bringup = true
[target]
serial = "/dev/unused"
boot_wait = 0
boot_timeout = "20s"
console_check = ["System ready", "part ok"]
[handoff]
method = "virtio"
command_timeout = "10s"
prompt_timeout = "30s"
commands = ["setenv loadaddr 0x80200000", "setenv fdt_addr_r 0x88000000", "virtio scan", "fatload virtio 0:1 ${loadaddr} kernel.bin", "fdt move ${fdtcontroladdr} ${fdt_addr_r} 0x20000", "booti ${loadaddr} - ${fdt_addr_r}"]
''')
    package(ROOT / vars_["KERNEL_ELF"], ROOT / vars_["KERNEL_BIN"], out / "bundle",
            source.board, fixture)
    image = out / "disk.img"
    fat = out / "boot.fat"
    size = 128 * 1024 * 1024
    with fat.open("wb") as f:
        f.truncate(size - PARTITION_OFFSET)
    mkfs = shutil.which("mkfs.fat") or "/sbin/mkfs.fat"
    subprocess.run([mkfs, "--invariant", "-F", "32", str(fat)], check=True)
    subprocess.run(["mcopy", "-i", str(fat), str(out / "bundle/kernel.bin"), "::/kernel.bin"], check=True)
    with image.open("wb") as f:
        f.truncate(size)
        f.write(mbr(size))
        f.seek(PARTITION_OFFSET)
        with fat.open("rb") as payload:
            shutil.copyfileobj(payload, f)
    master, slave = pty.openpty()
    transport = SerialTransport(os.ttyname(slave), 115200)
    process = None
    try:
        process = subprocess.Popen(["qemu-system-riscv64", "-machine", "virt", "-m", "1G",
            "-smp", "1", "-nographic", "-monitor", "none", "-no-reboot", "-bios", "default",
            "-kernel", str(args.uboot.resolve()),
            "-drive", f"if=none,id=a20disk,format=raw,file={image}",
            "-device", "virtio-blk-device,drive=a20disk"], stdin=master, stdout=master, stderr=master)
        inst = parse_instance(fixture)
        inst = replace(inst, target=replace(inst.target, serial=os.ttyname(slave)))
        result = run_image_handoff(transport, inst, out / "bundle", on_output=lambda t: print(t, end="", flush=True))
        write_console_log(out / "serial.log", result.transcript)
        if not result.ok:
            ap.exit(1, f"U-Boot/A20 serial handoff FAIL at {result.stage}: {result.missing}\n")
        print("\nReal U-Boot -> verified Image -> A20 kernel: PASS")
    finally:
        transport.close()
        if process is not None and process.poll() is None:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
        os.close(master)
        os.close(slave)


if __name__ == "__main__":
    main()

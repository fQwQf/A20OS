#!/usr/bin/env python3
"""Exercise ARM32 uImage handoff through real U-Boot and the production UART runner.

Requires qemu_arm_defconfig U-Boot and an already built smoke-arm32 kernel.
This tests the bootm protocol on QEMU virt, not V3s hardware.
"""
import argparse
from dataclasses import replace
import hashlib
import json
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
from lichee_pi_zero import elf_layout, validate_uimage
from pine_a64 import mbr, PARTITION_OFFSET


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--uboot", type=Path, required=True)
    ap.add_argument("--out", type=Path, default=ROOT / "build/hardware-validation/uboot-bootm-qemu")
    ap.add_argument("--virtualization", action="store_true",
                    help="start U-Boot with ARM virtualization enabled (HYP entry)")
    args = ap.parse_args()
    out = args.out.absolute()
    bundle = out / "bundle"
    bundle.mkdir(parents=True, exist_ok=True)
    source = parse_instance(ROOT / "instances/smoke-arm32.toml")
    variables = query_make(source, ("KERNEL_ELF", "KERNEL_BIN"))
    for name, key in (("kernel.elf", "KERNEL_ELF"), ("kernel.bin", "KERNEL_BIN")):
        shutil.copyfile(ROOT / variables[key], bundle / name)
    load, footprint = elf_layout((bundle / "kernel.elf").read_bytes())
    subprocess.run([str(args.uboot.resolve().parent / "tools/mkimage"), "-A", "arm",
                    "-O", "linux", "-T", "kernel", "-C", "none", "-a", hex(load),
                    "-e", hex(load), "-n", "A20 ARM32 protocol test", "-d",
                    str(bundle / "kernel.bin"), str(bundle / "kernel.uimg")], check=True)
    validate_uimage((bundle / "kernel.uimg").read_bytes(), (bundle / "kernel.bin").read_bytes())
    commands = ["setenv loadaddr 0x42000000", "setenv fdt_addr_r 0x41800000",
                "virtio scan", "fatload virtio 0:1 ${loadaddr} kernel.uimg",
                "fdt move ${fdtcontroladdr} ${fdt_addr_r} 0x100000",
                "bootm ${loadaddr} - ${fdt_addr_r}"]
    fixture = out / "qemu.toml"
    fixture.write_text('name="uboot-bootm-qemu"\narch="arm32"\nboard="lichee-pi-zero"\n'
                       '[kernel]\nbringup=true\n[target]\nserial="/dev/unused"\n'
                       'boot_wait=0\nboot_timeout="30s"\n'
                       'console_check=["System ready", "part ok"]\n'
                       '[handoff]\nmethod="virtio"\ncommand_timeout="10s"\nprompt_timeout="30s"\n'
                       'commands=' + json.dumps(commands) + '\n')
    (bundle / "uboot.cmd").write_text("\n".join(commands) + "\n")
    files = {p.name: {"size": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
             for p in bundle.iterdir() if p.name != "manifest.json"}
    (bundle / "manifest.json").write_text(json.dumps({
        "board": "lichee-pi-zero", "boot_protocol": "bootm", "payload_name": "kernel.uimg",
        "payload_load_address": "0x42000000", "load_address": hex(load), "image_size": footprint,
        "instance": {"name": "uboot-bootm-qemu", "sha256": hashlib.sha256(fixture.read_bytes()).hexdigest()},
        "files": files}))
    size = 128 * 1024 * 1024
    fat, image = out / "boot.fat", out / "disk.img"
    with fat.open("wb") as f:
        f.truncate(size - PARTITION_OFFSET)
    subprocess.run([shutil.which("mkfs.fat") or "/sbin/mkfs.fat", "--invariant", "-F", "32", str(fat)], check=True)
    subprocess.run(["mcopy", "-i", str(fat), str(bundle / "kernel.uimg"), "::/kernel.uimg"], check=True)
    with image.open("wb") as f:
        f.truncate(size)
        f.write(mbr(size))
        f.seek(PARTITION_OFFSET)
        with fat.open("rb") as data:
            shutil.copyfileobj(data, f)
    master, slave = pty.openpty()
    transport = SerialTransport(os.ttyname(slave), 115200)
    process = None
    try:
        machine = "virt,virtualization=on" if args.virtualization else "virt"
        # smoke-arm32's qemu-virt board exposes one GiB to its allocator.
        process = subprocess.Popen(["qemu-system-arm", "-machine", machine, "-cpu", "cortex-a15",
            "-m", "1G", "-smp", "1", "-nographic", "-monitor", "none", "-no-reboot",
            "-bios", str(args.uboot.resolve()), "-drive", f"if=none,id=disk,format=raw,file={image}",
            "-device", "virtio-blk-device,drive=disk"], stdin=master, stdout=master, stderr=master)
        inst = parse_instance(fixture)
        inst = replace(inst, target=replace(inst.target, serial=os.ttyname(slave)))
        result = run_image_handoff(transport, inst, bundle, on_output=lambda t: print(t, end="", flush=True))
        write_console_log(out / "serial.log", result.transcript)
        if not result.ok:
            ap.exit(1, f"U-Boot bootm/A20 FAIL at {result.stage}: {result.missing}\n")
        print("\nReal U-Boot -> verified ARM32 uImage -> A20 kernel: PASS")
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

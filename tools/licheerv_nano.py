#!/usr/bin/env python3
"""Build pinned SG2002 firmware and a FAT FIP SD image; never write devices."""
from __future__ import annotations

import argparse
import json
import os
from pathlib import Path
import shutil
import struct
import subprocess
import sys
import tempfile
import zlib

import pine_a64 as common

ROOT = common.ROOT
BASE = ROOT / "build/licheerv-nano"
DTB_NAME = "sg2002-licheerv-nano-b.dtb"
LOAD_ADDRESS = 0x80200000
FDT_ADDRESS = 0x82000000
SCRIPT_ADDRESS = 0x8E000000
SOURCES = {
    "u-boot": common.SOURCES["u-boot"],
    "opensbi": {
        "version": "v1.7", "repository": "riscv-software-src/opensbi",
        "commit": "a32a91069119e7a5aa31e6bc51d5e00860be3d80",
        "sha256": "01f8c8ce394a4d1656b83c2a97b0870c7137795bff1197a42600becaedd7dc9e",
    },
    "fiptool": {
        "repository": "sophgo/fiptool", "version": "2024-11-05 snapshot",
        "commit": "7f59889c91f7d5d440d6a09aad0209f0aca3d09d",
        "sha256": "26d9e847038b1611120ca6abcfee869ac15143230025e79772da27eac77dc821",
    },
}


def patch_environment(header: Path) -> None:
    original = header.read_text()
    patched = original
    for before, after in (("scriptaddr=0x80c00000", "scriptaddr=0x8e000000"),
                          ("kernel_addr_r=0x81000000", "kernel_addr_r=0x80200000")):
        if patched.count(before) != 1:
            raise common.PrepareError(f"unexpected upstream Nano environment: missing/ambiguous {before}")
        patched = patched.replace(before, after)
    header.write_text(patched)


def elf_span(path: Path) -> int:
    data = path.read_bytes()
    if data[:6] != b"\x7fELF\x02\x01" or len(data) < 64:
        raise common.PrepareError("expected ELF64 U-Boot")
    table = struct.unpack_from("<Q", data, 32)[0]
    step, count = struct.unpack_from("<HH", data, 54)
    ranges = []
    for i in range(count):
        offset = table + i * step
        if offset + 56 > len(data):
            raise common.PrepareError("truncated U-Boot ELF segments")
        if struct.unpack_from("<I", data, offset)[0] == 1:
            address = struct.unpack_from("<Q", data, offset + 24)[0]
            size = struct.unpack_from("<Q", data, offset + 40)[0]
            if size:
                ranges.append((address, address + size))
    if not ranges:
        raise common.PrepareError("U-Boot has no loadable memory span")
    return max(end for _, end in ranges) - min(start for start, _ in ranges)


def dtb_memory(blob: bytes) -> tuple[int, int]:
    """Read the pinned upstream DT's one-cell SG2002 memory tuple."""
    common.root_compatible(blob)  # Validate basic header/structure offsets.
    _, total, offset, strings = struct.unpack_from(">4I", blob)
    depth, memory = -1, False
    while offset + 4 <= total:
        token = struct.unpack_from(">I", blob, offset)[0]
        offset += 4
        if token == 1:
            end = blob.find(b"\0", offset, total)
            if end < 0:
                break
            depth += 1
            if depth == 1:
                memory = blob[offset:end] == b"memory@80000000"
            offset = (end + 4) & ~3
        elif token == 2:
            if depth == 1:
                memory = False
            depth -= 1
        elif token == 3 and offset + 8 <= total:
            length, name = struct.unpack_from(">2I", blob, offset)
            offset += 8
            end = blob.find(b"\0", strings + name, total)
            if end < 0 or offset + length > total:
                break
            if memory and depth == 1 and blob[strings + name:end] == b"reg":
                if length != 8:
                    break
                return struct.unpack_from(">2I", blob, offset)
            offset += (length + 3) & ~3
        elif token == 4:
            continue
        else:
            break
    raise common.PrepareError("Nano DTB must describe one-cell 256-MiB memory@80000000")


def validate_fip(blob: bytes) -> None:
    if len(blob) < 4096 or blob[:8] != b"CVBL01\n\0":
        raise common.PrepareError("not a Sophgo FIP param1 image")
    param2 = struct.unpack_from("<I", blob, 0xE0)[0]
    if param2 + 4096 > len(blob) or blob[param2:param2 + 8] != b"CVLD02\n\0":
        raise common.PrepareError("invalid Sophgo FIP param2 pointer")
    if struct.unpack_from("<I", blob, param2 + 40)[0] != 0:
        raise common.PrepareError("secondary RTOS payload must be disabled for this Nano profile")
    if struct.unpack_from("<I", blob, param2 + 60)[0] != 0x80000000:
        raise common.PrepareError("OpenSBI must start below the A20OS Image")


def validate_firmware(directory: Path) -> dict:
    manifest = json.loads((directory / "manifest.json").read_text())
    if manifest.get("board") != "licheerv-nano" or manifest.get("ram_mib") != 256:
        raise common.PrepareError("firmware must target SG2002 Nano 256 MiB")
    for name in ("fip.bin", DTB_NAME, "mkimage", "u-boot.bin"):
        if manifest.get("files", {}).get(name) != common.artifact(directory / name):
            raise common.PrepareError(f"changed/missing prepared Nano artifact: {name}")
    compat = common.root_compatible((directory / DTB_NAME).read_bytes())
    if "sipeed,licheerv-nano" not in compat or "sophgo,sg2002" not in compat:
        raise common.PrepareError("firmware DTB does not describe SG2002 LicheeRV Nano")
    if dtb_memory((directory / DTB_NAME).read_bytes()) != (0x80000000, 0x10000000):
        raise common.PrepareError("Nano DTB must describe exactly 256 MiB DDR")
    validate_fip((directory / "fip.bin").read_bytes())
    if b"scriptaddr=0x8e000000\0" not in (directory / "u-boot.bin").read_bytes():
        raise common.PrepareError("U-Boot default script buffer still overlaps the A20OS Image")
    if manifest.get("rtos_size") != 0 or manifest.get("script_address") != SCRIPT_ADDRESS:
        raise common.PrepareError("unsafe Nano firmware layout/RTOS settings; rerun prepare")
    return manifest


def prepare(args) -> None:
    make = common.tool("make")
    gcc = common.tool(args.cross_compile + "gcc")
    for name in ("bison", "flex", "swig", "openssl"):
        common.tool(name)
    env = common.firmware_environment()
    work, out = args.work.resolve(), args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    pydeps = work / "python-deps"
    if not (pydeps / "elftools").is_dir():
        common.run([sys.executable, "-m", "pip", "install", "--target", str(pydeps),
                    "--disable-pip-version-check", "pyelftools==0.32"])
    env["PYTHONPATH"] = str(pydeps) + os.pathsep + env.get("PYTHONPATH", "")
    sources = {name: common.source_tree(name, args.cache.resolve(), spec)
               for name, spec in SOURCES.items()}
    uboot, opensbi, fiptool = (sources[name] for name in ("u-boot", "opensbi", "fiptool"))
    env_header = uboot / "include/configs/licheerv_nano.h"
    patch_environment(env_header)
    ub_out, sbi_out = work / "u-boot", work / "opensbi"
    command = [make, "-C", str(uboot), f"O={ub_out}", f"CROSS_COMPILE={args.cross_compile}",
               f"PYTHON={sys.executable}", f"PYTHON3={sys.executable}"]
    common.run([*command, "sipeed_licheerv_nano_defconfig"], env=env)
    common.run([str(uboot / "scripts/config"), "--file", str(ub_out / ".config"),
                "--enable", "CRC32_VERIFY", "--enable", "BOOTMETH_SCRIPT"], env=env)
    common.run([*command, "olddefconfig"], env=env)
    common.run([*command, f"-j{args.jobs}"], env=env)
    dtbs = list(ub_out.rglob(DTB_NAME))
    if not dtbs or len({common.sha256(path) for path in dtbs}) != 1:
        raise common.PrepareError("missing/ambiguous SG2002 Nano B DTB")
    dtb = dtbs[0]
    common.run([make, "-C", str(opensbi), f"O={sbi_out}", "PLATFORM=generic",
                f"FW_FDT_PATH={dtb}", f"CROSS_COMPILE={args.cross_compile}",
                f"-j{args.jobs}"], env=env)
    # Mainline generic firmware occupies bottom DDR; the runtime U-Boot
    # relocates at the top. Reject builds exceeding a conservative 16-MiB
    # high-RAM budget, including 0x820000 malloc and 2 MiB stack/GD/FDT margin.
    reserved_budget = elf_span(ub_out / "u-boot") + 0x820000 + 0x200000
    if reserved_budget > 0x1000000:
        raise common.PrepareError("U-Boot high-RAM budget reaches Nano script staging window")
    empty_rtos = work / "rtos-disabled.bin"
    empty_rtos.write_bytes(b"")
    out.mkdir(parents=True, exist_ok=True)
    common.regular_output(out / "fip.bin")
    fw_dynamic = sbi_out / "platform/generic/firmware/fw_dynamic.bin"
    common.run([sys.executable, str(fiptool / "fiptool"),
                "--fsbl", str(fiptool / "data/fsbl/cv181x.bin"),
                "--ddr_param", str(fiptool / "data/ddr_param.bin"),
                "--opensbi", str(fw_dynamic), "--uboot", str(ub_out / "u-boot.bin"),
                "--rtos", str(empty_rtos), str(out / "fip.bin")], env=env)
    outputs = {DTB_NAME: dtb, "mkimage": ub_out / "tools/mkimage",
               "fw_dynamic.bin": fw_dynamic, "u-boot.bin": ub_out / "u-boot.bin",
               "cv181x.bin": fiptool / "data/fsbl/cv181x.bin",
               "ddr_param.bin": fiptool / "data/ddr_param.bin",
               "fiptool-LICENSE": fiptool / "LICENSE"}
    for name, source in outputs.items():
        common.regular_output(out / name)
        shutil.copy2(source, out / name)
    manifest = {
        "format": "a20os-licheerv-nano-firmware-v1", "board": "licheerv-nano", "ram_mib": 256,
        "hardware_validated": False, "sources": SOURCES,
        "defconfig": "sipeed_licheerv_nano_defconfig", "opensbi_platform": "generic",
        "config_overrides": ["CONFIG_CRC32_VERIFY=y", "CONFIG_BOOTMETH_SCRIPT=y"],
        "script_address": SCRIPT_ADDRESS, "fdt_address": FDT_ADDRESS,
        "kernel_address": LOAD_ADDRESS, "uboot_reserved_budget": reserved_budget,
        "environment_header_sha256": common.sha256(env_header), "rtos_size": 0,
        "source_date_epoch": common.SOURCE_DATE_EPOCH,
        "compiler": common.run([gcc, "--version"], capture=True).splitlines()[0],
        "files": {name: common.artifact(out / name) for name in [*outputs, "fip.bin"]},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    validate_firmware(out)
    print(f"Prepared SG2002 Nano firmware (no secondary RTOS): {out}")


def boot_script(kernel: bytes, dtb: bytes) -> str:
    return f'''# SG2002 Nano SD/UART bring-up; no Ethernet claim.
echo A20OS LicheeRV Nano SD boot
setenv loadaddr 0x{LOAD_ADDRESS:x}
setenv fdt_addr_r 0x{FDT_ADDRESS:x}
setenv bootargs a20.hostname=licheerv-nano
if fatload mmc 0:1 ${{loadaddr}} kernel.bin && itest ${{filesize}} == 0x{len(kernel):x} && crc32 -v ${{loadaddr}} ${{filesize}} {zlib.crc32(kernel):08x}; then
    if fatload mmc 0:1 ${{fdt_addr_r}} {DTB_NAME} && itest ${{filesize}} == 0x{len(dtb):x} && crc32 -v ${{fdt_addr_r}} ${{filesize}} {zlib.crc32(dtb):08x}; then
        booti ${{loadaddr}} - ${{fdt_addr_r}}
    fi
fi
echo A20OS Nano SD boot failed - inspect files and load results
exit 1
'''


def sdcard(args) -> None:
    firmware, bundle, output = args.firmware.resolve(), args.bundle.resolve(), args.output.absolute()
    common.regular_output(output)
    fw = validate_firmware(firmware)
    kernel = (bundle / "kernel.bin").read_bytes()
    tree = (firmware / DTB_NAME).read_bytes()
    meta = json.loads((bundle / "manifest.json").read_text())
    if (meta.get("board") != "licheerv-nano" or len(kernel) < 64 or
            kernel[48:56] != b"RISCV\0\0\0" or kernel[56:60] != b"RSC\x05"):
        raise common.PrepareError("expected SG2002 Nano RISC-V Image bundle")
    offset, footprint = struct.unpack_from("<QQ", kernel, 8)
    if (int(meta["load_address"], 0) != LOAD_ADDRESS or offset != 0x200000 or
            footprint < len(kernel) or LOAD_ADDRESS + footprint >= FDT_ADDRESS or
            meta["files"]["kernel.bin"] != common.artifact(bundle / "kernel.bin")):
        raise common.PrepareError("Nano Image load/footprint/digest overlaps firmware staging or is invalid")
    shutil.copy2(firmware / DTB_NAME, bundle / DTB_NAME)
    meta["files"][DTB_NAME] = common.artifact(bundle / DTB_NAME)
    (bundle / "manifest.json").write_text(json.dumps(meta, indent=2) + "\n")
    mkfs = common.tool("mkfs.fat", "/usr/sbin/mkfs.fat", "/sbin/mkfs.fat")
    mcopy = common.tool("mcopy")
    image_size = args.size_mib * 1024 * 1024
    partition_table = common.mbr(image_size)
    env = dict(os.environ, SOURCE_DATE_EPOCH=common.SOURCE_DATE_EPOCH, TZ="UTC", MTOOLS_SKIP_CHECK="1")
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".nano-") as directory:
        stage = Path(directory)
        fat = stage / "boot.fat"
        with fat.open("wb") as stream:
            stream.truncate(image_size - common.PARTITION_OFFSET)
        common.run([mkfs, "--invariant", "-F", "32", "-n", "A20NANO", str(fat)], env=env)
        (stage / "kernel.bin").write_bytes(kernel)
        (stage / DTB_NAME).write_bytes(tree)
        (stage / "boot.cmd").write_text(boot_script(kernel, tree))
        common.run([str(firmware / "mkimage"), "-A", "riscv", "-O", "linux", "-T", "script",
                    "-C", "none", "-n", "A20OS SG2002 Nano", "-d", str(stage / "boot.cmd"),
                    str(stage / "boot.scr")], env=env)
        files = {"fip.bin": firmware / "fip.bin", "kernel.bin": stage / "kernel.bin",
                 DTB_NAME: stage / DTB_NAME, "boot.cmd": stage / "boot.cmd", "boot.scr": stage / "boot.scr"}
        verify = stage / "verify"
        verify.mkdir()
        for name, source in files.items():
            common.run([mcopy, "-i", str(fat), str(source), "::/" + name], env=env)
            common.run([mcopy, "-i", str(fat), "::/" + name, str(verify / name)], env=env)
            if common.sha256(source) != common.sha256(verify / name):
                raise common.PrepareError(f"FAT read-back mismatch: {name}")
        image = stage / "sdcard.img"
        with image.open("wb") as stream:
            stream.truncate(image_size)
            stream.write(partition_table)
            # SG2002 BootROM reads FAT fip.bin. No Allwinner raw SPL write.
            stream.seek(common.PARTITION_OFFSET)
            with fat.open("rb") as contents:
                shutil.copyfileobj(contents, stream)
        manifest = {"format": "a20os-licheerv-nano-sdcard-v1", "board": "licheerv-nano", "ram_mib": 256,
                    "hardware_validated": False, "image": common.artifact(image),
                    "partition_offset": common.PARTITION_OFFSET, "raw_firmware_offset": None,
                    "firmware": fw, "kernel": meta,
                    "fat_files": {name: common.artifact(path) for name, path in files.items()}}
        image.replace(output)
        output.with_suffix(output.suffix + ".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"SG2002 Nano SD image ready: {output} (no physical device written)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    actions = parser.add_subparsers(dest="action", required=True)
    p = actions.add_parser("prepare")
    p.add_argument("--cache", type=Path, default=BASE / "source-cache")
    p.add_argument("--work", type=Path, default=BASE / "firmware-build")
    p.add_argument("--out", type=Path, default=BASE / "firmware")
    p.add_argument("--cross-compile", default="riscv64-linux-gnu-")
    p.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    p.set_defaults(func=prepare)
    p = actions.add_parser("sdcard")
    p.add_argument("--firmware", type=Path, default=BASE / "firmware")
    p.add_argument("--bundle", type=Path, default=BASE / "handoff")
    p.add_argument("--output", type=Path, default=BASE / "a20os-licheerv-nano-sdcard.img")
    p.add_argument("--size-mib", type=int, default=128)
    p.set_defaults(func=sdcard)
    args = parser.parse_args()
    try:
        args.func(args)
    except (common.PrepareError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"licheerv-nano: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

#!/usr/bin/env python3
"""Prepare pinned PINE A64 firmware and assemble an unprivileged SD image.

Only regular files are produced. This tool never opens a physical block device.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import stat
import struct
import subprocess
import sys
import tarfile
import tempfile
import urllib.request
import zlib

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "build/pine-a64"
SOURCES = {
    "tf-a": {
        "version": "v2.13.0",
        "commit": "c17351450c8a513ca3f30f936e26a71db693a145",
        "repository": "TrustedFirmware-A/trusted-firmware-a",
        "sha256": "f17ecbc0d900ba3c168f583415073988beb1cc74067290a3a67884543755f611",
    },
    "u-boot": {
        "version": "v2025.10",
        "commit": "e50b1e8715011def8aff1588081a2649a2c6cd47",
        "repository": "u-boot/u-boot",
        "sha256": "33b6910dc737bfbbf4317ae0a48fe687335d6a08eee8fdfcb3a4adffc4c4a516",
    },
}
SECTOR = 512
FIRMWARE_OFFSET = 8192
PARTITION_OFFSET = 8 * 1024 * 1024
LOAD_ADDRESS = 0x40080000
FDT_ADDRESS = 0x4FA00000
DTB_NAME = "sun50i-a64-pine64.dtb"
SOURCE_DATE_EPOCH = "1759778025"  # U-Boot v2025.10 tag timestamp.


class PrepareError(RuntimeError):
    pass


def sha256(path: Path) -> str:
    with path.open("rb") as stream:
        return hashlib.file_digest(stream, "sha256").hexdigest()


def artifact(path: Path) -> dict:
    return {"size": path.stat().st_size, "sha256": sha256(path)}


def run(argv: list[str], *, env=None, capture=False) -> str:
    print("+ " + " ".join(map(str, argv)), flush=True)
    result = subprocess.run(argv, check=True, env=env, text=True,
                            stdout=subprocess.PIPE if capture else None)
    return result.stdout.strip() if capture else ""


def tool(name: str, *fallbacks: str) -> str:
    found = shutil.which(name)
    if found:
        return found
    for path in fallbacks:
        if Path(path).is_file() and os.access(path, os.X_OK):
            return path
    raise PrepareError(f"missing host tool {name}; install the dependency described in docs/platforms/pine-a64.md")


def regular_output(path: Path) -> None:
    if path.is_symlink() or (path.exists() and not stat.S_ISREG(path.stat().st_mode)):
        raise PrepareError(f"output must be a regular file, never a device/symlink: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)


def source_tree(name: str, cache: Path, spec: dict | None = None) -> Path:
    spec = SOURCES[name] if spec is None else spec
    cache.mkdir(parents=True, exist_ok=True)
    stem = f"{name}-{spec['commit'][:12]}"
    archive = cache / (stem + ".tar.gz")
    if not archive.exists():
        url = f"https://codeload.github.com/{spec['repository']}/tar.gz/{spec['commit']}"
        print(f"Downloading {url}", flush=True)
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as target:
            tmp = Path(target.name)
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    shutil.copyfileobj(response, target)
                target.close()
                if sha256(tmp) != spec["sha256"]:
                    raise PrepareError(f"download digest mismatch for {name}; refusing unpinned source")
                tmp.replace(archive)
            finally:
                tmp.unlink(missing_ok=True)
    if sha256(archive) != spec["sha256"]:
        raise PrepareError(f"cached source archive changed: {archive}")
    destination = cache / stem
    # Re-extract the verified archive rather than trusting editable cache files.
    # Build products live in a separate output tree and are preserved.
    with tempfile.TemporaryDirectory(dir=cache, prefix="extract-") as directory:
        with tarfile.open(archive) as packed:
            packed.extractall(directory, filter="data")
        roots = list(Path(directory).iterdir())
        if len(roots) != 1 or not roots[0].is_dir():
            raise PrepareError(f"unexpected {name} source archive layout")
        if destination.exists():
            shutil.rmtree(destination)
        roots[0].rename(destination)
    return destination.resolve()


def root_compatible(blob: bytes) -> list[str]:
    if len(blob) < 40:
        raise PrepareError("truncated DTB")
    magic, total, start, strings = struct.unpack_from(">4I", blob)
    if magic != 0xD00DFEED or total > len(blob) or not 40 <= start < total or not 40 <= strings < total:
        raise PrepareError("invalid DTB header")
    offset, depth = start, -1
    while offset + 4 <= total:
        token = struct.unpack_from(">I", blob, offset)[0]
        offset += 4
        if token == 1:
            end = blob.find(b"\0", offset, total)
            if end < 0:
                break
            offset = (end + 4) & ~3
            depth += 1
        elif token == 2:
            depth -= 1
        elif token == 3 and offset + 8 <= total:
            length, name_offset = struct.unpack_from(">2I", blob, offset)
            offset += 8
            name_start = strings + name_offset
            end = blob.find(b"\0", name_start, total)
            if name_start >= total or end < 0 or offset + length > total:
                break
            if depth == 0 and blob[name_start:end] == b"compatible":
                return blob[offset:offset + length].rstrip(b"\0").decode("ascii").split("\0")
            offset += (length + 3) & ~3
        elif token == 4:
            continue
        elif token == 9:
            break
        else:
            break
    raise PrepareError("DTB has no valid root compatible")


def validate_firmware(firmware: Path) -> dict:
    manifest = json.loads((firmware / "manifest.json").read_text())
    if manifest.get("board") != "pine-a64" or manifest.get("ram_mib") != 512:
        raise PrepareError("firmware manifest is not for original PINE A64 512 MiB")
    for name in ("u-boot-sunxi-with-spl.bin", DTB_NAME, "mkimage"):
        path = firmware / name
        if manifest.get("files", {}).get(name) != artifact(path):
            raise PrepareError(f"firmware artifact does not match prepared digest: {name}")
    spl = (firmware / "u-boot-sunxi-with-spl.bin").read_bytes()
    if spl[4:12] != b"eGON.BT0" or len(spl) > PARTITION_OFFSET - FIRMWARE_OFFSET:
        raise PrepareError("invalid/oversized Allwinner SPL image")
    if "pine64,pine64" not in root_compatible((firmware / DTB_NAME).read_bytes()):
        raise PrepareError("wrong DTB: original 512 MiB board requires pine64,pine64")
    return manifest


def firmware_environment(epoch: str = SOURCE_DATE_EPOCH) -> dict[str, str]:
    """Start an independent firmware build, including when invoked by A20 make."""
    env = dict(os.environ, SOURCE_DATE_EPOCH=epoch, TZ="UTC")
    # U-Boot chooses ARCH=arm/riscv itself. A20's ARCH names and command-line
    # make overrides must not change this separate project's configuration.
    for name in ("ARCH", "MAKEFLAGS", "MFLAGS", "MAKEOVERRIDES", "MAKELEVEL"):
        env.pop(name, None)
    return env


def prepare(args) -> None:
    make = tool("make")
    gcc = tool(args.cross_compile + "gcc")
    for name in ("bison", "flex", "swig", "openssl"):
        tool(name)
    env = firmware_environment()
    work, out = args.work.resolve(), args.out.resolve()
    work.mkdir(parents=True, exist_ok=True)
    # U-Boot binman uses pyelftools. Keep its pinned dependency local instead
    # of changing the user's interpreter or site-packages.
    pydeps = work / "python-deps"
    if not (pydeps / "elftools").is_dir():
        run([sys.executable, "-m", "pip", "install", "--target", str(pydeps),
             "--disable-pip-version-check", "pyelftools==0.32"])
    env["PYTHONPATH"] = str(pydeps) + os.pathsep + env.get("PYTHONPATH", "")
    tfa = source_tree("tf-a", args.cache.resolve())
    uboot = source_tree("u-boot", args.cache.resolve())
    tfa_build, uboot_build = work / "tf-a", work / "u-boot"
    run([make, "-C", str(tfa), f"BUILD_BASE={tfa_build}", "PLAT=sun50i_a64",
         "DEBUG=1", f"CROSS_COMPILE={args.cross_compile}", f"-j{args.jobs}", "bl31"], env=env)
    bl31 = tfa_build / "sun50i_a64/debug/bl31.bin"
    common = [make, "-C", str(uboot), f"O={uboot_build}",
              f"CROSS_COMPILE={args.cross_compile}", f"PYTHON={sys.executable}",
              f"PYTHON3={sys.executable}", f"BL31={bl31}", "SCP=/dev/null"]
    run([*common, "pine64_plus_defconfig"], env=env)
    # Keep original board/DRAM selection. These two command features let the
    # generated script verify payload bytes under either distro/bootstd scan.
    run([str(uboot / "scripts/config"), "--file", str(uboot_build / ".config"),
         "--enable", "CRC32_VERIFY", "--enable", "BOOTMETH_SCRIPT"], env=env)
    run([*common, "olddefconfig"], env=env)
    run([*common, f"-j{args.jobs}"], env=env)
    dtbs = list(uboot_build.rglob(DTB_NAME))
    if not dtbs or len({sha256(path) for path in dtbs}) != 1:
        raise PrepareError("missing/ambiguous original PINE A64 DTB in U-Boot output")
    out.mkdir(parents=True, exist_ok=True)
    outputs = {"bl31.bin": bl31, "u-boot-sunxi-with-spl.bin": uboot_build / "u-boot-sunxi-with-spl.bin",
               DTB_NAME: dtbs[0], "mkimage": uboot_build / "tools/mkimage"}
    for name, source in outputs.items():
        regular_output(out / name)
        shutil.copy2(source, out / name)
    manifest = {
        "format": "a20os-pine-a64-firmware-v1", "board": "pine-a64", "ram_mib": 512,
        "hardware_validated": False, "sources": SOURCES, "defconfig": "pine64_plus_defconfig",
        "tf_a_platform": "sun50i_a64", "scp": None,
        "config_overrides": ["CONFIG_CRC32_VERIFY=y", "CONFIG_BOOTMETH_SCRIPT=y"],
        "source_date_epoch": SOURCE_DATE_EPOCH,
        "compiler": run([gcc, "--version"], capture=True).splitlines()[0],
        "python": sys.version.splitlines()[0],
        "files": {name: artifact(out / name) for name in outputs},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    validate_firmware(out)
    print(f"Prepared original PINE A64 512 MiB firmware: {out}")


def mbr(image_size: int) -> bytes:
    if image_size < 72 * 1024 * 1024 or image_size % SECTOR:
        raise PrepareError("SD image must be sector-aligned and at least 72 MiB (FAT32)")
    start, count = PARTITION_OFFSET // SECTOR, (image_size - PARTITION_OFFSET) // SECTOR
    if start + count > 0xFFFFFFFF:
        raise PrepareError("image exceeds MBR LBA capacity")
    sector = bytearray(SECTOR)
    struct.pack_into("<I", sector, 440, 0x41323050)
    struct.pack_into("<B3sB3sII", sector, 446, 0x80, b"\xfe\xff\xff", 0x0C,
                     b"\xfe\xff\xff", start, count)
    sector[510:512] = b"\x55\xaa"
    return bytes(sector)


def boot_script(kernel: bytes, dtb: bytes, load: int = LOAD_ADDRESS) -> str:
    return f'''# Original PINE A64 512 MiB, generated by A20OS.
echo A20OS PINE A64 SD boot
setenv loadaddr 0x{load:x}
setenv fdt_addr_r 0x{FDT_ADDRESS:x}
setenv bootargs a20.hostname=pine-a64
if fatload mmc 0:1 ${{loadaddr}} kernel.bin && itest ${{filesize}} == 0x{len(kernel):x} && crc32 -v ${{loadaddr}} ${{filesize}} {zlib.crc32(kernel):08x}; then
    if fatload mmc 0:1 ${{fdt_addr_r}} {DTB_NAME} && itest ${{filesize}} == 0x{len(dtb):x} && crc32 -v ${{fdt_addr_r}} ${{filesize}} {zlib.crc32(dtb):08x}; then
        booti ${{loadaddr}} - ${{fdt_addr_r}}
    fi
fi
echo A20OS SD boot failed - inspect files and load results
exit 1
'''


def sdcard(args) -> None:
    firmware, bundle, output = args.firmware.resolve(), args.bundle.resolve(), args.output.absolute()
    regular_output(output)
    fw_manifest = validate_firmware(firmware)
    kernel = bundle / "kernel.bin"
    meta = json.loads((bundle / "manifest.json").read_text())
    kernel_data = kernel.read_bytes()
    dtb_data = (firmware / DTB_NAME).read_bytes()
    header = kernel_data[:64]
    if meta.get("board") != "pine-a64" or header[56:60] != b"ARMd" or len(header) < 64:
        raise PrepareError("kernel bundle must be a PINE A64 ARM64 Image")
    offset, footprint = struct.unpack_from("<QQ", header, 8)
    if (int(meta["load_address"], 0) != LOAD_ADDRESS or offset != 0x80000 or
            footprint < kernel.stat().st_size or LOAD_ADDRESS + footprint >= FDT_ADDRESS or
            meta["files"]["kernel.bin"] != artifact(kernel)):
        raise PrepareError("kernel load address, footprint or bundle digest is invalid")
    if "CONFIG_CRC32_VERIFY=y" not in fw_manifest.get("config_overrides", []):
        raise PrepareError("prepared U-Boot lacks required CRC verification; rerun prepare")
    shutil.copy2(firmware / DTB_NAME, bundle / DTB_NAME)
    meta["files"][DTB_NAME] = artifact(bundle / DTB_NAME)
    (bundle / "manifest.json").write_text(json.dumps(meta, indent=2) + "\n")
    mkfs = tool("mkfs.fat", "/usr/sbin/mkfs.fat", "/sbin/mkfs.fat")
    mcopy = tool("mcopy")
    image_size = args.size_mib * 1024 * 1024
    partition_table = mbr(image_size)
    env = dict(os.environ, SOURCE_DATE_EPOCH=SOURCE_DATE_EPOCH, TZ="UTC", MTOOLS_SKIP_CHECK="1")
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".pine-a64-") as directory:
        stage = Path(directory)
        fat = stage / "boot.fat"
        with fat.open("wb") as stream:
            stream.truncate(image_size - PARTITION_OFFSET)
        run([mkfs, "--invariant", "-F", "32", "-n", "A20BOOT", str(fat)], env=env)
        # Snapshot payloads before computing boot checksums/copying to FAT.
        (stage / "kernel.bin").write_bytes(kernel_data)
        (stage / DTB_NAME).write_bytes(dtb_data)
        (stage / "boot.cmd").write_text(boot_script(kernel_data, dtb_data))
        run([str(firmware / "mkimage"), "-A", "arm64", "-O", "linux", "-T", "script",
             "-C", "none", "-n", "A20OS PINE A64", "-d", str(stage / "boot.cmd"),
             str(stage / "boot.scr")], env=env)
        files = {"kernel.bin": stage / "kernel.bin", DTB_NAME: stage / DTB_NAME,
                 "boot.cmd": stage / "boot.cmd", "boot.scr": stage / "boot.scr"}
        for name, source in files.items():
            run([mcopy, "-i", str(fat), str(source), "::/" + name], env=env)
        verify = stage / "verify"
        verify.mkdir()
        for name, source in files.items():
            run([mcopy, "-i", str(fat), "::/" + name, str(verify / name)], env=env)
            if sha256(verify / name) != sha256(source):
                raise PrepareError(f"FAT read-back mismatch: {name}")
        temporary_image = stage / "sdcard.img"
        with temporary_image.open("wb") as stream:
            stream.truncate(image_size)
            stream.write(partition_table)
            stream.seek(FIRMWARE_OFFSET)
            stream.write((firmware / "u-boot-sunxi-with-spl.bin").read_bytes())
            stream.seek(PARTITION_OFFSET)
            with fat.open("rb") as partition:
                shutil.copyfileobj(partition, stream)
        manifest = {"format": "a20os-pine-a64-sdcard-v1", "board": "pine-a64", "ram_mib": 512,
                    "hardware_validated": False, "image": artifact(temporary_image),
                    "firmware_offset": FIRMWARE_OFFSET, "partition_offset": PARTITION_OFFSET,
                    "firmware": fw_manifest, "kernel": meta,
                    "fat_files": {name: artifact(path) for name, path in files.items()}}
        temporary_image.replace(output)
        output.with_suffix(output.suffix + ".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"PINE A64 SD image ready: {output} ({image_size} bytes; no device written)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("prepare", help="download digest-pinned source and build SPL/TF-A/U-Boot/DTB")
    p.add_argument("--cache", type=Path, default=BASE / "source-cache")
    p.add_argument("--work", type=Path, default=BASE / "firmware-build")
    p.add_argument("--out", type=Path, default=BASE / "firmware")
    p.add_argument("--cross-compile", default="aarch64-linux-gnu-")
    p.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    p.set_defaults(func=prepare)
    p = sub.add_parser("sdcard", help="assemble a regular-file MBR/FAT32 SD image without root/loop mounts")
    p.add_argument("--firmware", type=Path, default=BASE / "firmware")
    p.add_argument("--bundle", type=Path, default=BASE / "handoff")
    p.add_argument("--output", type=Path, default=BASE / "a20os-pine-a64-sdcard.img")
    p.add_argument("--size-mib", type=int, default=128)
    p.set_defaults(func=sdcard)
    args = parser.parse_args()
    try:
        args.func(args)
    except (PrepareError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"pine-a64: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

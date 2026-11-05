#!/usr/bin/env python3
"""Prepare pinned Quartz64-Zero RK3566T firmware and assemble an unprivileged SD image.

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
from pine_a64 import firmware_environment

ROOT = Path(__file__).resolve().parents[2]
BASE = ROOT / "build/quartz64-zero"
SOURCES = {
    "u-boot": {
        "version": "v2025.10",
        "commit": "e50b1e8715011def8aff1588081a2649a2c6cd47",
        "repository": "u-boot/u-boot",
        "sha256": "33b6910dc737bfbbf4317ae0a48fe687335d6a08eee8fdfcb3a4adffc4c4a516",
    },
}
VIDEO_SOURCE = {
    "version": "v2026.07", "commit": "ece349ade2973e220f524ce59e59711cc919263f",
    "repository": "u-boot/u-boot",
    "sha256": "c51a3c160ee8a7f4304e52498ecfe327789c9464a3681b5c5bd3c72b1f18a079",
}
VIDEO_FILES = ROOT / "tools/firmware/quartz64-zero-video"
VIDEO_CONFIG = ("CONFIG_VIDEO=y", "CONFIG_VIDEO_ROCKCHIP=y",
                "CONFIG_DISPLAY_ROCKCHIP_HDMI=y", "CONFIG_VIDEO_DT_SIMPLEFB=y",
                "CONFIG_EVENT=y", "# CONFIG_VIDEO_REMOVE is not set",
                "CONFIG_PMIC_RK8XX=y", "CONFIG_REGULATOR_RK8XX=y")

SECTOR = 512
FIRMWARE_OFFSET = 32768
PARTITION_OFFSET = 16 * 1024 * 1024
LOAD_ADDRESS = 0x02080000
FDT_ADDRESS = 0x0A000000
DTB_NAME = "rk3566-quartz64-zero.dtb"
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
    raise PrepareError(f"missing host tool {name}; install the dependency described in docs/platforms/quartz64-zero.md")


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


RKBIN_COMMIT = "3e288fe814e059dd06833495f845cab04ac20a5c"
BLOBS = {
    "rk3566_ddr_1056MHz_v1.26.bin": "9966c21aadf00737ccecc2fad6fe3c902ff26956433029f70dfd759d4f60616a",
    "rk3568_bl31_v1.46.elf": "c81ac7e8e1fd727cf7f0db62a9aaea760bde2b270e34d98eb264a264b86df749",
}
LOCAL_FIRMWARE = ROOT / "tools/firmware/quartz64-zero"


def blob_file(cache: Path, name: str) -> Path:
    cache.mkdir(parents=True, exist_ok=True)
    target = cache / name
    if not target.exists():
        url = f"https://raw.githubusercontent.com/rockchip-linux/rkbin/{RKBIN_COMMIT}/bin/rk35/{name}"
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                with urllib.request.urlopen(url, timeout=60) as response:
                    shutil.copyfileobj(response, stream)
                stream.close()
                if sha256(temporary) != BLOBS[name]:
                    raise PrepareError(f"Rockchip blob digest mismatch: {name}")
                temporary.replace(target)
            finally:
                temporary.unlink(missing_ok=True)
    if target.is_symlink() or sha256(target) != BLOBS[name]:
        raise PrepareError(f"modified Rockchip blob: {target}")
    return target.resolve()


def validate_firmware(firmware: Path) -> dict:
    manifest = json.loads((firmware / "manifest.json").read_text())
    if manifest.get("board") != "quartz64-zero" or manifest.get("ram_mib") != 1024:
        raise PrepareError("firmware manifest is not for Quartz64-Zero RK3566T 1 GiB")
    for name in ("u-boot-rockchip.bin", DTB_NAME, "mkimage"):
        if manifest.get("files", {}).get(name) != artifact(firmware / name):
            raise PrepareError(f"modified firmware artifact: {name}")
    if manifest.get("video_profile"):
        config = firmware / "u-boot.config"
        if manifest.get("files", {}).get("u-boot.config") != artifact(config):
            raise PrepareError("modified video firmware configuration")
        if not set(VIDEO_CONFIG).issubset(config.read_text().splitlines()):
            raise PrepareError("video firmware lacks required scanout/handoff configuration")
    data = (firmware / "u-boot-rockchip.bin").read_bytes()
    # RK3568 SD v2 header, then FIT at configured SPL_PAD_TO (relative to 32KiB).
    if (data[:4] != b"RKNS" or len(data) <= 0x7f8000 or
            data[0x7f8000:0x7f8004] != b"\xd0\x0d\xfe\xed" or
            len(data) > PARTITION_OFFSET - FIRMWARE_OFFSET):
        raise PrepareError("invalid/oversized Rockchip SD image or wrong FIT offset")
    if "pine64,quartz64-zero" not in root_compatible((firmware / DTB_NAME).read_bytes()):
        raise PrepareError("DTB is not the independent Quartz64-Zero board description")
    return manifest


def uboot_environment() -> dict[str, str]:
    return firmware_environment(SOURCE_DATE_EPOCH)


def prepare(args) -> None:
    make, gcc = tool("make"), tool(args.cross_compile + "gcc")
    for name in ("bison", "flex", "swig", "openssl"):
        tool(name)
    env = uboot_environment()
    video = getattr(args, "video", False)
    work, out, cache = args.work.resolve(), args.out.resolve(), args.cache.resolve()
    if video:
        if work == (BASE / "firmware-build").resolve():
            work = BASE / "firmware-build-video"
        if out == (BASE / "firmware").resolve():
            out = BASE / "firmware-video"
    work.mkdir(parents=True, exist_ok=True)
    pydeps = work / "python-deps"
    if not (pydeps / "elftools").is_dir():
        run([sys.executable, "-m", "pip", "install", "--target", str(pydeps),
             "--disable-pip-version-check", "pyelftools==0.32"])
    env["PYTHONPATH"] = str(pydeps) + os.pathsep + env.get("PYTHONPATH", "")
    source_spec = VIDEO_SOURCE if video else SOURCES["u-boot"]
    uboot = source_tree("u-boot", cache, source_spec)
    if video:
        for entry in json.loads((VIDEO_FILES / "series.json").read_text()):
            patch = VIDEO_FILES / "patches" / entry["file"]
            if sha256(patch) != entry["sha256"]:
                raise PrepareError(f"modified video patch: {patch}")
            run([tool("patch"), "--batch", "--fuzz=0", "-d", str(uboot),
                 "-p1", "-i", str(patch)], env=env)
        # This VOP2 pipeline programs native ARGB8888 without channel swap.
        vop_source = uboot / "drivers/video/rockchip/rk_vop2.c"
        vop_text = vop_source.read_text()
        needle = "uc_priv->bpix = l2bpp;"
        if vop_text.count(needle) != 1:
            raise PrepareError("unexpected VOP2 format publication site")
        vop_text = vop_text.replace(needle, needle + "\n\tuc_priv->format = VIDEO_X8R8G8B8;")
        routing = "writel(reg, &overlay->port_sel);"
        if vop_text.count(routing) != 1:
            raise PrepareError("unexpected VOP2 plane routing site")
        vop_text = vop_text.replace(routing, routing +
            "\n\tif (port != 0) return -ENOSYS;\n\ta20_vop2_opaque(priv->regs);")
        vop_text = vop_text.replace('#include "rk_vop2.h"',
            '#include "rk_vop2.h"\n#include "a20_vop2_opaque.h"')
        vop_source.write_text(vop_text)
        shutil.copy2(VIDEO_FILES / "a20_vop2_opaque.h", vop_source.parent)
        dt_dir = uboot / "dts/upstream/src/arm64/rockchip"
        shutil.copy2(VIDEO_FILES / "rk3566-quartz64-zero-video.dtsi", dt_dir)
        shutil.copy2(VIDEO_FILES / "a20_simplefb.c", uboot / "arch/arm/mach-rockchip/a20_simplefb.c")
        with (uboot / "arch/arm/mach-rockchip/Makefile").open("a") as stream:
            stream.write("\nifndef CONFIG_XPL_BUILD\nobj-y += a20_simplefb.o\nendif\n")
    for path in LOCAL_FIRMWARE.iterdir():
        target_dir = uboot / ("dts/upstream/src/arm64/rockchip" if path.suffix == ".dts" else "arch/arm/dts")
        shutil.copy2(path, target_dir / path.name)
    if video:
        board_dts = uboot / "dts/upstream/src/arm64/rockchip" / (DTB_NAME[:-4] + ".dts")
        with board_dts.open("a") as stream:
            stream.write('\n#include "rk3566-quartz64-zero-video.dtsi"\n')
    tpl = blob_file(cache, "rk3566_ddr_1056MHz_v1.26.bin")
    bl31 = blob_file(cache, "rk3568_bl31_v1.46.elf")
    uboot_build = work / "u-boot"
    common = [make, "-C", str(uboot), f"O={uboot_build}",
              f"CROSS_COMPILE={args.cross_compile}", f"PYTHON={sys.executable}",
              f"PYTHON3={sys.executable}", f"BL31={bl31}", f"ROCKCHIP_TPL={tpl}"]
    run([*common, "generic-rk3568_defconfig"], env=env)
    config = [str(uboot / "scripts/config"), "--file", str(uboot_build / ".config")]
    options = {
        "OF_UPSTREAM": True, "DEFAULT_DEVICE_TREE": "rockchip/rk3566-quartz64-zero",
        "DEFAULT_FDT_FILE": DTB_NAME,
        "OF_LIST": "rockchip/rk3566-quartz64-zero", "SPL_OF_LIST": "rockchip/rk3566-quartz64-zero", "ROCKCHIP_SPI_IMAGE": False,
        "SPL_SPI_FLASH_SUPPORT": False, "SPL_SPI": False, "SPL_SPI_LOAD": False,
        "ROCKCHIP_SFC": False, "USB": False, "USB_GADGET": False,
        "CMD_ROCKUSB": False, "CMD_USB_MASS_STORAGE": False,
        "CRC32_VERIFY": True, "BOOTMETH_SCRIPT": True,
        "CMD_FAT": True, "CMD_ITEST": True, "CMD_BOOTI": True,
        "SYS_MALLOC_LEN": "0x1000000",
    }
    if video:
        options.update({"VIDEO": True, "DISPLAY": True, "VIDEO_ROCKCHIP": True,
                        "DISPLAY_ROCKCHIP_HDMI": True, "VIDEO_DT_SIMPLEFB": True,
                        "EVENT": True, "VIDEO_REMOVE": False, "CMD_VIDEO": True,
                        "DM_PMIC": True, "PMIC_RK8XX": True,
                        "DM_REGULATOR": True, "REGULATOR_RK8XX": True,
                        "VIDEO_ROCKCHIP_MAX_XRES": "3840",
                        "VIDEO_ROCKCHIP_MAX_YRES": "2160"})
    for key, value in options.items():
        run([*config, "--enable" if value is True else "--disable" if value is False else
             "--set-str" if not value.startswith("0x") and not value.isdigit() else "--set-val", key,
             *([] if isinstance(value, bool) else [value])], env=env)
    run([*common, "olddefconfig"], env=env)
    run([*common, f"-j{args.jobs}"], env=env)
    if video:
        actual_config = (uboot_build / ".config").read_text().splitlines()
        if not set(VIDEO_CONFIG).issubset(actual_config):
            raise PrepareError("U-Boot rejected a required video configuration option")
    dtb = uboot_build / "dts/upstream/src/arm64/rockchip" / DTB_NAME
    out.mkdir(parents=True, exist_ok=True)
    outputs = {"u-boot-rockchip.bin": uboot_build / "u-boot-rockchip.bin",
               DTB_NAME: dtb, "mkimage": uboot_build / "tools/mkimage"}
    if video:
        outputs["u-boot.config"] = uboot_build / ".config"
    for name, source in outputs.items():
        regular_output(out / name)
        shutil.copy2(source, out / name)
    manifest = {
        "format": "a20os-quartz64-zero-firmware-v1", "board": "quartz64-zero", "ram_mib": 1024,
        "hardware_validated": False, "sources": {"u-boot": source_spec},
        "video_profile": video,
        "video_sources": ({"patches": json.loads((VIDEO_FILES / "series.json").read_text()),
                           "local_files": {p.name: artifact(p) for p in VIDEO_FILES.iterdir() if p.is_file()}} if video else None),
        "rkbin_commit": RKBIN_COMMIT, "rkbin_files": BLOBS,
        "defconfig": "generic-rk3568_defconfig", "config_overrides": ["CONFIG_CRC32_VERIFY=y", "CONFIG_BOOTMETH_SCRIPT=y"],
        "local_board_files": {path.name: artifact(path) for path in LOCAL_FIRMWARE.iterdir()},
        "source_date_epoch": SOURCE_DATE_EPOCH,
        "compiler": run([gcc, "--version"], capture=True).splitlines()[0],
        "files": {name: artifact(out / name) for name in outputs},
    }
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    validate_firmware(out)
    print(f"Quartz64-Zero firmware built (physical validation pending): {out}")

def mbr(image_size: int) -> bytes:
    if image_size < 80 * 1024 * 1024 or image_size % SECTOR:
        raise PrepareError("SD image must be sector-aligned and at least 80 MiB (FAT32)")
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
    return f'''# Quartz64-Zero RK3566T 1 GiB, generated by A20OS.
echo A20OS Quartz64-Zero SD boot
setenv loadaddr 0x{load:x}
setenv fdt_addr_r 0x{FDT_ADDRESS:x}
setenv bootargs a20.hostname=quartz64-zero
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
    if meta.get("board") != "quartz64-zero" or header[56:60] != b"ARMd" or len(header) < 64:
        raise PrepareError("kernel bundle must be a Quartz64-Zero ARM64 Image")
    offset, footprint, flags = struct.unpack_from("<QQQ", header, 8)
    if (int(meta["load_address"], 0) != LOAD_ADDRESS or offset != 0x80000 or
            flags != 10 or footprint < kernel.stat().st_size or LOAD_ADDRESS + footprint >= FDT_ADDRESS or
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
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".quartz64-zero-") as directory:
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
             "-C", "none", "-n", "A20OS Quartz64-Zero", "-d", str(stage / "boot.cmd"),
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
            stream.write((firmware / "u-boot-rockchip.bin").read_bytes())
            stream.seek(PARTITION_OFFSET)
            with fat.open("rb") as partition:
                shutil.copyfileobj(partition, stream)
        manifest = {"format": "a20os-quartz64-zero-sdcard-v1", "board": "quartz64-zero", "ram_mib": 1024,
                    "hardware_validated": False, "image": artifact(temporary_image),
                    "firmware_offset": FIRMWARE_OFFSET, "partition_offset": PARTITION_OFFSET,
                    "firmware": fw_manifest, "kernel": meta,
                    "fat_files": {name: artifact(path) for name, path in files.items()}}
        temporary_image.replace(output)
        output.with_suffix(output.suffix + ".json").write_text(json.dumps(manifest, indent=2) + "\n")
    print(f"Quartz64-Zero SD image ready: {output} ({image_size} bytes; no device written)")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("prepare", help="download digest-pinned source and build SPL/TF-A/U-Boot/DTB")
    p.add_argument("--video", action="store_true", help="opt-in RK3566 VOP2/HDMI firmware; separate firmware-video output")
    p.add_argument("--cache", type=Path, default=BASE / "source-cache")
    p.add_argument("--work", type=Path, default=BASE / "firmware-build")
    p.add_argument("--out", type=Path, default=BASE / "firmware")
    p.add_argument("--cross-compile", default="aarch64-linux-gnu-")
    p.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 1))
    p.set_defaults(func=prepare)
    p = sub.add_parser("sdcard", help="assemble a regular-file MBR/FAT32 SD image without root/loop mounts")
    p.add_argument("--firmware", type=Path, default=BASE / "firmware")
    p.add_argument("--bundle", type=Path, default=BASE / "handoff")
    p.add_argument("--output", type=Path, default=BASE / "a20os-quartz64-zero-sdcard.img")
    p.add_argument("--size-mib", type=int, default=128)
    p.set_defaults(func=sdcard)
    args = parser.parse_args()
    try:
        args.func(args)
    except (PrepareError, OSError, ValueError, KeyError, subprocess.CalledProcessError) as error:
        print(f"quartz64-zero: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

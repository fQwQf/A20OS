#!/usr/bin/env python3
"""Ox64 D0 payload media and pinned OpenBouffalo SPI recovery assets.

prepare/sdcard only create regular files. recovery prints ROM UART commands;
real SPI writes require both --execute and --flash-mib 16.
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

ROOT = Path(__file__).resolve().parents[1]
BASE = ROOT / "build/ox64"
RELEASE = "v1.0.1"
ARCHIVE = "bl808-linux-pine64_ox64_defconfig.tar.gz"
URL = f"https://github.com/openbouffalo/buildroot_bouffalo/releases/download/{RELEASE}/{ARCHIVE}"
ARCHIVE_SHA256 = "da97c6b59bd3c0eb4caf60f4963ef9ebace55c19091a46f3a745790742f76e3e"
SPI = {
    "m0_lowload_bl808_m0.bin": (0x0, "f9d5a1b38367b0e6f85691c395caa79987dc92f8e08cad0314f39a88e86d629d"),
    "d0_lowload_bl808_d0.bin": (0x100000, "2a662c9467fa2dfb56f585be7d60cdfd491640360b43fd09414ff21906beba22"),
    "bl808-firmware.bin": (0x800000, "447d6b818d98690c9bdb13ebe4b97a75cd3642838ab535d6b389bf1e1551c6f3"),
}
DTB_NAME = "bl808-pine64-ox64.dtb"
LOAD = 0x50200000
FDT = 0x53800000
M0_RESERVED = 0x53f00000
SCRATCH = 0x50010000
PARTITION_OFFSET = 8 * 1024 * 1024


class Ox64Error(ValueError):
    pass


def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


def artifact(path: Path) -> dict:
    data = path.read_bytes()
    return {"size": len(data), "sha256": digest(data)}


def regular(path: Path) -> None:
    if path.is_symlink() or (path.exists() and not stat.S_ISREG(path.stat().st_mode)):
        raise Ox64Error(f"output must be a regular file, never a device/symlink: {path}")
    path.parent.mkdir(parents=True, exist_ok=True)


def firmware_dtb(firmware: bytes) -> bytes:
    if len(firmware) != 7 * 1024 * 1024 or struct.unpack_from("<4I", firmware) != (0x4c4d5642, 1, 3, 0):
        raise Ox64Error("not the supported BL808 lowload firmware container")
    for i in range(3):
        magic, kind, offset, size = struct.unpack_from("<4I", firmware, 16 + 16 * i)
        if magic != 0x5c2381b2 or offset + size > len(firmware) or not size:
            raise Ox64Error("invalid firmware section")
        if kind == 1:
            blob = firmware[offset:offset + size]
            # Keep vendor clock/interrupt/CPU nodes. Add the documented M0
            # PSRAM reservation absent from this older release's DTB.
            return reserve_m0(blob)
    raise Ox64Error("firmware has no DTB")


def reserve_m0(blob: bytes) -> bytes:
    if len(blob) < 40:
        raise Ox64Error("short firmware DTB")
    h = list(struct.unpack_from(">10I", blob))
    magic, total, structure, strings, reserve = h[:5]
    if magic != 0xd00dfeed or total != len(blob) or reserve < 40 or reserve % 8:
        raise Ox64Error("invalid firmware DTB header")
    if not reserve <= structure < strings <= total or b"pine64,ox64\0" not in blob:
        raise Ox64Error("DTB is not a supported Ox64 tree")
    pos = reserve
    while pos + 16 <= structure:
        base, size = struct.unpack_from(">QQ", blob, pos)
        if not base and not size:
            break
        if size == 0 or base + size >= 1 << 64:
            raise Ox64Error("invalid DTB memreserve entry")
        pos += 16
    else:
        raise Ox64Error("DTB reservation terminator missing")
    # Insert before the terminator; all later offsets and total grow by 16.
    result = blob[:pos] + struct.pack(">QQ", M0_RESERVED, 0x100000) + blob[pos:]
    h[1] += 16
    h[2] += 16
    h[3] += 16
    return struct.pack(">10I", *h) + result[40:]


def validate_assets(folder: Path) -> dict:
    manifest = json.loads((folder / "manifest.json").read_text())
    if manifest.get("format") != "a20os-ox64-firmware-v1" or manifest.get("release") != RELEASE:
        raise Ox64Error("run ox64-prepare for the supported firmware release")
    for name, (_, expected) in SPI.items():
        if digest((folder / name).read_bytes()) != expected:
            raise Ox64Error(f"prepared SPI firmware changed: {name}")
    expected_dtb = firmware_dtb((folder / "bl808-firmware.bin").read_bytes())
    if (folder / DTB_NAME).read_bytes() != expected_dtb:
        raise Ox64Error("prepared Ox64 DTB changed")
    return manifest


def prepare(args) -> None:
    out, cache = args.out.absolute(), args.cache.absolute()
    cache.mkdir(parents=True, exist_ok=True)
    archive = cache / ARCHIVE
    if not archive.exists():
        regular(archive)
        with tempfile.NamedTemporaryFile(dir=cache, delete=False) as stream:
            temporary = Path(stream.name)
            try:
                print(f"Downloading {URL}", flush=True)
                with urllib.request.urlopen(URL, timeout=60) as response:
                    shutil.copyfileobj(response, stream)
                stream.close()
                if digest(temporary.read_bytes()) != ARCHIVE_SHA256:
                    raise Ox64Error("release archive digest mismatch")
                temporary.replace(archive)
            finally:
                temporary.unlink(missing_ok=True)
    if digest(archive.read_bytes()) != ARCHIVE_SHA256:
        raise Ox64Error("cached release archive digest mismatch")
    out.mkdir(parents=True, exist_ok=True)
    with tarfile.open(archive, "r:gz") as packed:
        for name, (_, expected) in SPI.items():
            member = packed.getmember("firmware/" + name)
            if not member.isfile() or member.size > 8 * 1024 * 1024:
                raise Ox64Error("unexpected release archive member")
            data = packed.extractfile(member).read()
            if digest(data) != expected:
                raise Ox64Error(f"release member digest mismatch: {name}")
            destination = out / name
            regular(destination)
            destination.write_bytes(data)
    dtb = firmware_dtb((out / "bl808-firmware.bin").read_bytes())
    regular(out / DTB_NAME)
    (out / DTB_NAME).write_bytes(dtb)
    manifest = {"format": "a20os-ox64-firmware-v1", "board": "ox64", "release": RELEASE,
                "source": URL, "archive_sha256": ARCHIVE_SHA256, "hardware_validated": False,
                "flash_mib": 16, "spi_offsets": {k: hex(v[0]) for k, v in SPI.items()},
                "dtb_adjustment": "reserve PSRAM 0x53f00000..0x54000000 for M0",
                "files": {name: artifact(out / name) for name in [*SPI, DTB_NAME]}}
    regular(out / "manifest.json")
    (out / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
    regular(out / "RECOVERY.txt")
    (out / "RECOVERY.txt").write_text(
        "Ox64 16 MiB SPI recovery (not for the 2 MiB board):\n"
        "Power off; wire 3.3V ROM UART GPIO14 TX(pin1) -> adapter RX,\n"
        "GPIO15 RX(pin2) <- adapter TX, GND. Hold BOOT during power-on/reset.\n"
        "Install bflb-iot-tool==1.10.0 (Python 3.13 also needs standard-telnetlib==3.13.0).\n"
        "Inspect commands with:\n"
        "  python3 tools/ox64.py recovery --port /dev/ttyUSB0\n"
        "Then explicitly write these verified assets with:\n"
        "  python3 tools/ox64.py recovery --port /dev/ttyUSB0 --flash-mib 16 --execute\n"
        "After success power off, release BOOT, move UART to GPIO16/17 (pins32/31),\n"
        "insert prepared SD, power on, capture 2000000 baud console.\n"
        "This overwrites SPI boot firmware. Preserve any customized firmware first.\n"
        "See docs/platforms/ox64.md for adapters and failure recovery.\n")
    print(f"Ox64 firmware/DTB ready: {out}; SPI was not written")


def recovery(args) -> None:
    folder = args.firmware.absolute()
    validate_assets(folder)
    commands = [[args.tool, "--chipname", "bl808", "--interface", "uart", "--port", args.port,
                 "--baudrate", str(args.baud), "--addr", hex(offset), "--firmware", str(folder / name), "--single"]
                for name, (offset, _) in SPI.items()]
    print(json.dumps({"writes_spi": bool(args.execute), "flash_mib_required": 16,
                      "rom_uart_gpio": [14, 15], "commands": commands}, indent=2), flush=True)
    if args.execute:
        if args.flash_mib != 16:
            raise Ox64Error("SPI write requires explicit --flash-mib 16; 2 MiB boards are unsupported")
        if not shutil.which(args.tool):
            raise Ox64Error(f"install {args.tool} before requesting SPI recovery")
        logs = args.log_dir.absolute()
        logs.mkdir(parents=True, exist_ok=True)
        for index, command in enumerate(commands):
            run_flasher(command, logs / f"{index + 1}-{Path(command[-2]).stem}.log")


def run_flasher(command: list[str], log: Path) -> None:
    # bflb-iot-tool 1.10.0 does not propagate some download failures to its
    # process exit code. Require its eflash completion marker as well.
    regular(log)
    success = False
    with log.open("w") as transcript:
        process = subprocess.Popen(command, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                   text=True, bufsize=1)
        try:
            assert process.stdout is not None
            for line in process.stdout:
                print(line, end="", flush=True)
                transcript.write(line)
                transcript.flush()
                if "[All Successful]" in line:
                    success = True
            status = process.wait()
        except BaseException:
            process.terminate()
            try:
                process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.wait()
            raise
        finally:
            if process.stdout is not None:
                process.stdout.close()
    if status != 0 or not success:
        raise Ox64Error(f"ROM SPI download was not confirmed (exit {status}); stop and inspect {log}")


def script_image(text: str) -> bytes:
    # U-Boot legacy script: 32-bit length list + zero terminator, then source.
    source = text.encode("ascii")
    payload = struct.pack(">II", len(source), 0) + source
    fields = [0x27051956, 0, 0, len(payload), 0, 0, zlib.crc32(payload)]
    tail = bytes([5, 26, 6, 0]) + b"A20OS Ox64 D0".ljust(32, b"\0")
    header = struct.pack(">7I", *fields) + tail
    fields[1] = zlib.crc32(header)
    return struct.pack(">7I", *fields) + tail + payload


def boot_script(kernel: bytes, dtb: bytes) -> str:
    # v1.0.1's U-Boot uses the compact crc32 implementation (no -v), which
    # stores native ulong CRC at the requested address. Match it with itest.l.
    # The scratch range 0x50010000..08 is before the kernel and beyond the script.
    return f'''# OpenBouffalo {RELEASE}, Ox64 16 MiB flash board.
echo A20OS Ox64 D0 SD boot
setenv loadaddr 0x{LOAD:x}
setenv fdt_addr_r 0x{FDT:x}
setenv fdt_high 0xffffffffffffffff
setenv bootargs a20.hostname=ox64
if mmc rescan && fatload mmc 0:1 ${{loadaddr}} kernel.bin && itest ${{filesize}} == 0x{len(kernel):x}; then
    if crc32 ${{loadaddr}} ${{filesize}} 0x{SCRATCH:x} && itest.l *0x{SCRATCH:x} == 0x{zlib.crc32(kernel):08x}; then
        if fatload mmc 0:1 ${{fdt_addr_r}} {DTB_NAME} && itest ${{filesize}} == 0x{len(dtb):x}; then
            if crc32 ${{fdt_addr_r}} ${{filesize}} 0x{SCRATCH:x} && itest.l *0x{SCRATCH:x} == 0x{zlib.crc32(dtb):08x}; then
                booti ${{loadaddr}} - ${{fdt_addr_r}}
            fi
        fi
    fi
fi
echo A20OS Ox64 SD boot failed - stop and inspect firmware/files
exit 1
'''


def mbr(image_size: int) -> bytes:
    if image_size < 72 << 20 or image_size % 512 or image_size // 512 > 0xffffffff:
        raise Ox64Error("SD image must be sector-aligned, 72 MiB or larger, within MBR capacity")
    result = bytearray(512)
    struct.pack_into("<I", result, 440, 0x4132304f)
    struct.pack_into("<B3sB3sII", result, 446, 0x80, b"\xfe\xff\xff", 0x0c, b"\xfe\xff\xff",
                     PARTITION_OFFSET // 512, (image_size - PARTITION_OFFSET) // 512)
    result[510:] = b"\x55\xaa"
    return bytes(result)


def sdcard(args) -> None:
    folder, bundle, output = args.firmware.absolute(), args.bundle.absolute(), args.output.absolute()
    fw = validate_assets(folder)
    regular(output)
    kernel = (bundle / "kernel.bin").read_bytes()
    dtb = (folder / DTB_NAME).read_bytes()
    meta = json.loads((bundle / "manifest.json").read_text())
    if len(kernel) < 64 or kernel[48:60] != b"RISCV\0\0\0RSC\x05":
        raise Ox64Error("bundle lacks the RISC-V booti Image header")
    offset, footprint = struct.unpack_from("<QQ", kernel, 8)
    if (meta.get("board") != "ox64" or int(meta["load_address"], 0) != LOAD or offset != 0x200000 or
            footprint < len(kernel) or LOAD + footprint >= FDT or FDT + len(dtb) >= M0_RESERVED or
            meta["files"]["kernel.bin"] != artifact(bundle / "kernel.bin")):
        raise Ox64Error("invalid board/load/footprint/digest or DTB overlaps Image/M0 PSRAM")
    # Bind the matching DTB to the same handoff manifest used by a20.
    regular(bundle / DTB_NAME)
    (bundle / DTB_NAME).write_bytes(dtb)
    meta["files"][DTB_NAME] = artifact(bundle / DTB_NAME)
    regular(bundle / "manifest.json")
    (bundle / "manifest.json").write_text(json.dumps(meta, indent=2) + "\n")
    image_size = args.size_mib << 20
    table = mbr(image_size)
    mkfs = shutil.which("mkfs.fat") or next((p for p in ["/usr/sbin/mkfs.fat", "/sbin/mkfs.fat"] if Path(p).is_file()), None)
    mcopy = shutil.which("mcopy")
    if not mkfs or not mcopy:
        raise Ox64Error("install dosfstools and mtools")
    env = dict(os.environ, SOURCE_DATE_EPOCH="0", TZ="UTC", MTOOLS_SKIP_CHECK="1")
    with tempfile.TemporaryDirectory(dir=output.parent, prefix=".ox64-") as directory:
        stage = Path(directory)
        fat = stage / "boot.fat"
        with fat.open("wb") as stream:
            stream.truncate(image_size - PARTITION_OFFSET)
        subprocess.run([mkfs, "--invariant", "-F", "32", "-n", "A20OX64", str(fat)], env=env, check=True)
        (stage / "kernel.bin").write_bytes(kernel)
        (stage / DTB_NAME).write_bytes(dtb)
        text = boot_script(kernel, dtb)
        (stage / "boot.cmd").write_text(text)
        (stage / "boot.scr").write_bytes(script_image(text))
        files = ["kernel.bin", DTB_NAME, "boot.cmd", "boot.scr"]
        verify = stage / "verify"
        verify.mkdir()
        for name in files:
            subprocess.run([mcopy, "-i", str(fat), str(stage / name), "::/" + name], env=env, check=True)
            subprocess.run([mcopy, "-i", str(fat), "::/" + name, str(verify / name)], env=env, check=True)
            if artifact(stage / name) != artifact(verify / name):
                raise Ox64Error(f"FAT readback mismatch: {name}")
        image = stage / "sdcard.img"
        with image.open("wb") as stream:
            stream.truncate(image_size)
            stream.write(table)
            stream.seek(PARTITION_OFFSET)
            with fat.open("rb") as partition:
                shutil.copyfileobj(partition, stream)
        report = {"format": "a20os-ox64-sdcard-v1", "board": "ox64", "hardware_validated": False,
                  "requires_spi_firmware": f"OpenBouffalo {RELEASE} (preserved; not in SD image)",
                  "partition_offset": PARTITION_OFFSET, "image": artifact(image), "kernel": meta,
                  "firmware": fw, "fat_files": {n: artifact(stage / n) for n in files}}
        image.replace(output)
        regular(output.with_suffix(output.suffix + ".json"))
        output.with_suffix(output.suffix + ".json").write_text(json.dumps(report, indent=2) + "\n")
    print(f"Ox64 SD image ready: {output}; SPI and physical SD were not written")


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    sub = parser.add_subparsers(dest="action", required=True)
    p = sub.add_parser("prepare", help="download hash-pinned SPI recovery assets and prepare matching DTB")
    p.add_argument("--cache", type=Path, default=BASE / "source-cache")
    p.add_argument("--out", type=Path, default=BASE / "firmware")
    p.set_defaults(func=prepare)
    p = sub.add_parser("recovery", help="print ROM UART SPI recovery commands; explicit --execute writes SPI")
    p.add_argument("--firmware", type=Path, default=BASE / "firmware")
    p.add_argument("--port", default="/dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=115200)
    p.add_argument("--tool", default="bflb-iot-tool")
    p.add_argument("--flash-mib", type=int, choices=[16])
    p.add_argument("--execute", action="store_true")
    p.add_argument("--log-dir", type=Path, default=BASE / "recovery-logs")
    p.set_defaults(func=recovery)
    p = sub.add_parser("sdcard", help="build regular-file SD payload image for preserved SPI U-Boot")
    p.add_argument("--firmware", type=Path, default=BASE / "firmware")
    p.add_argument("--bundle", type=Path, default=BASE / "handoff")
    p.add_argument("--output", type=Path, default=BASE / "a20os-ox64-sdcard.img")
    p.add_argument("--size-mib", type=int, default=128)
    p.set_defaults(func=sdcard)
    a = parser.parse_args()
    try:
        a.func(a)
    except (OSError, ValueError, KeyError, TypeError, struct.error, tarfile.TarError, subprocess.CalledProcessError) as exc:
        parser.exit(1, f"ox64: {exc}\n")
    return 0

if __name__ == "__main__":
    raise SystemExit(main())

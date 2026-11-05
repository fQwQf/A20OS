#!/usr/bin/env python3
"""Flash a verified Hi3861 kernel A image over the ROM serial loader.

The Hi3861 ROM exposes a small hiburn/YMODEM interface after reset.  This tool
loads a pinned loaderboot into RAM, asks it to erase the kernel A partition at
0xd000, writes the image, and resets the SoC.  It never writes flash offset 0,
so the factory ROM/FlashBoot and NV partitions are left alone.

`prepare` downloads the loaderboot blob used by the open Hi3861 bootstrap
project; `--loader` can point at an equivalent signed loaderboot extracted from
the vendor SDK instead.
"""

from __future__ import annotations

import argparse
import hashlib
import struct
import sys
import time
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent.parent
OUT = ROOT / "build/hi3861"
IMAGE = OUT / "handoff/a20os.hupg"
LOADER = OUT / "loader.bin"
LOADER_URL = "https://raw.githubusercontent.com/xingrz/hi3861-bootstrap/master/tools/loader.bin"
LOADER_SHA256 = "af1e010d71880aa8a250ae51e5ce49f2c63cafac03864125af82ac58fd2dd103"
DEFAULT_ADDRESS = 0x00D000

DEADBEEF = b"\xef\xbe\xad\xde"
CMD_DL_IMAGE = 0xD2
CMD_RUN_RAM = 0xF0
CMD_RESET = 0x87
ACK_ROM = b"\xe1\x1e\x5a\x42"
ACK_SUCCESS = b"\xe1\x1e\x5a\xa5"
Y_SOH = b"\x01"
Y_STX = b"\x02"
Y_EOT = b"\x04"
Y_ACK = b"\x06"


def crc16(data: bytes) -> int:
    crc = 0
    for byte in data:
        crc ^= byte << 8
        for _ in range(8):
            crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return crc


def palindrome(value: int) -> int:
    return ((~value & 0xFF) << 8) | (value & 0xFF)


def make_cmd(payload: bytes) -> bytes:
    cmd = bytearray(DEADBEEF)
    cmd += struct.pack("<H", 4 + 2 + len(payload) + 2)
    cmd += payload
    cmd += struct.pack("<H", crc16(bytes(cmd)))
    return bytes(cmd)


def cmd_run_ram(baud: int) -> bytes:
    return make_cmd(struct.pack("<HII", palindrome(CMD_RUN_RAM), baud, 0x0108))


def cmd_download(address: int, size: int, erase: int) -> bytes:
    return make_cmd(struct.pack("<HIIIH", palindrome(CMD_DL_IMAGE),
                                address, size, erase, 0))


def cmd_reset() -> bytes:
    return make_cmd(struct.pack("<H", CMD_RESET))


def _align_up(value: int, align: int) -> int:
    return (value + align - 1) & ~(align - 1)


def _acks(data: bytes) -> list[bytes]:
    found: list[bytes] = []
    while True:
        at = data.find(DEADBEEF)
        if at < 0:
            return found
        data = data[at:]
        if len(data) < 6:
            return found
        length = struct.unpack_from("<H", data, 4)[0]
        if len(data) < length:
            return found
        packet = data[:length]
        data = data[length:]
        if crc16(packet[:-2]) == struct.unpack_from("<H", packet, length - 2)[0]:
            found.append(packet[6:-2])


def _wait_ack(ser, expected: bytes, timeout: float, label: str) -> None:
    deadline = time.time() + timeout
    seen = bytearray()
    while time.time() < deadline:
        chunk = ser.read(1024)
        if chunk:
            seen += chunk
            if expected in chunk or expected in seen:
                return
    raise TimeoutError(f"{label}: no {expected.hex()} response; device said "
                       f"{seen.decode('ascii', 'replace')[-400:]!r}")


def _wait_for_c(ser, timeout: float) -> None:
    """Wait until the YMODEM receiver advertises CRC mode with a 'C'."""
    deadline = time.time() + timeout
    seen = bytearray()
    while time.time() < deadline:
        chunk = ser.read(1024)
        if not chunk:
            continue
        seen += chunk
        if b"C" in seen:
            return
    raise TimeoutError("YMODEM receiver did not send its 'C' handshake")


def _loady(ser, name: str, data: bytes) -> None:
    """Send one YMODEM file, accepting the loaderboot's 1024-byte blocks."""
    def packet(seq: int, payload: bytes, wide: bool) -> bytes:
        body = payload.ljust(1024 if wide else 128, b"\0")
        out = bytearray(Y_STX if wide else Y_SOH)
        out += struct.pack("<H", palindrome(seq))
        out += body
        out += struct.pack(">H", crc16(body))
        return bytes(out)

    header = packet(0, f"{name}\0{len(data)}\0".encode(), False)
    for _ in range(6):
        _wait_for_c(ser, 10.0)
        ser.write(header)
        ser.flush()
        try:
            _wait_ack(ser, Y_ACK, 3.0, f"load {name} header")
            break
        except TimeoutError:
            continue
    else:
        raise TimeoutError(f"load {name}: header was not acknowledged")
    for index, offset in enumerate(range(0, len(data), 1024), start=1):
        block = data[offset:offset + 1024]
        for _ in range(6):
            ser.write(packet(index & 0xFF, block, True))
            ser.flush()
            try:
                _wait_ack(ser, Y_ACK, 3.0, f"load {name} block {index}")
                break
            except TimeoutError:
                continue
        else:
            raise TimeoutError(f"load {name}: block {index} was not acknowledged")
    ser.write(Y_EOT)
    ser.flush()
    _wait_ack(ser, Y_ACK + Y_ACK + b"C", 10.0, f"load {name} EOT")
    ser.write(packet(0, b"", False))
    ser.flush()
    _wait_ack(ser, Y_ACK, 5.0, f"load {name} close")


def _connect(ser, baud: int, timeout: float) -> None:
    print("waiting for the ROM loader; press RESET on the board now", flush=True)
    deadline = time.time() + timeout
    seen = bytearray()
    while time.time() < deadline:
        ser.write(cmd_run_ram(baud))
        ser.flush()
        chunk = ser.read(1024)
        if chunk:
            seen += chunk
            if ACK_ROM in seen:
                print("ROM loader connected", flush=True)
                return
            if b"Ready to load" in seen or b"CCC" in seen:
                print("loaderboot is already running", flush=True)
                return
            if len(seen) > 8192:
                del seen[:-4096]
        time.sleep(0.05)
    raise TimeoutError("ROM loader did not answer; check RESET, UART0 GPIO3/4 and port permissions")


def _prepare(loader: Path, source: str | None) -> None:
    if source:
        data = Path(source).read_bytes()
    else:
        print(f"downloading {LOADER_URL}", flush=True)
        with urllib.request.urlopen(LOADER_URL, timeout=30) as response:
            data = response.read()
    digest = hashlib.sha256(data).hexdigest()
    if digest != LOADER_SHA256:
        raise ValueError(f"loader SHA256 is {digest}, expected {LOADER_SHA256}")
    loader.parent.mkdir(parents=True, exist_ok=True)
    loader.write_bytes(data)
    print(f"loader ready: {loader} ({len(data)} bytes, sha256 {digest})")


def _serial(port: str, baud: int):
    try:
        import serial
    except ImportError as exc:
        raise ValueError("install pyserial (python3 -m pip install pyserial)") from exc
    return serial.Serial(port, baud, timeout=0.05)


def flash(port: str, baud: int, loader: Path, image: Path, address: int,
          dry_run: bool, timeout: float) -> None:
    from hi3861 import verify as verify_image

    verify_image() if image == IMAGE else None
    payload = image.read_bytes()
    if not payload:
        raise ValueError(f"{image} is empty")
    if address != DEFAULT_ADDRESS:
        raise ValueError(f"refusing non-kernel-A address 0x{address:x}; use 0x{DEFAULT_ADDRESS:x}")
    erase = _align_up(len(payload), 4096)
    for path in (loader, image):
        if not path.is_file():
            raise ValueError(f"missing {path}")
    if dry_run:
        print(f"would load {loader} ({loader.stat().st_size} bytes) into Hi3861 RAM")
        print(f"would erase 0x{address:05x}..0x{address + erase:05x} and write {image} "
              f"({len(payload)} bytes) at 0x{address:05x}")
        return

    ser = _serial(port, baud)
    try:
        _connect(ser, baud, timeout)
        ser.write(b"\x03\x00")
        ser.flush()
        time.sleep(0.2)
        print("loading loaderboot", flush=True)
        _loady(ser, loader.name, loader.read_bytes())
        _wait_ack(ser, ACK_SUCCESS, 10.0, "loaderboot start")
        print(f"erasing kernel A at 0x{address:05x} and writing {len(payload)} bytes", flush=True)
        ser.write(cmd_download(address, len(payload), erase))
        ser.flush()
        time.sleep(3.2)
        _wait_ack(ser, ACK_SUCCESS, 10.0, "kernel A download start")
        _loady(ser, image.name, payload)
        _wait_ack(ser, ACK_SUCCESS, 10.0, "kernel A download finish")
        print("kernel A written", flush=True)
        ser.write(cmd_reset())
        ser.flush()
        time.sleep(0.2)
    finally:
        ser.close()


def probe(port: str, baud: int, timeout: float) -> None:
    ser = _serial(port, baud)
    try:
        _connect(ser, baud, timeout)
    finally:
        ser.close()


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("action", choices=("prepare", "probe", "flash"))
    ap.add_argument("--port", default="/dev/ttyUSB0")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--loader", type=Path, default=LOADER)
    ap.add_argument("--loader-source", help="copy this signed loaderboot instead of downloading")
    ap.add_argument("--image", type=Path, default=IMAGE)
    ap.add_argument("--address", type=lambda s: int(s, 0), default=DEFAULT_ADDRESS)
    ap.add_argument("--timeout", type=float, default=30.0)
    ap.add_argument("--dry-run", action="store_true")
    args = ap.parse_args()
    try:
        if args.action == "prepare":
            _prepare(args.loader, args.loader_source)
        elif args.action == "probe":
            if args.dry_run:
                print(f"would probe {args.port} at {args.baud} baud")
            else:
                probe(args.port, args.baud, args.timeout)
        else:
            flash(args.port, args.baud, args.loader, args.image,
                  args.address, args.dry_run, args.timeout)
        return 0
    except (OSError, ValueError, TimeoutError) as exc:
        print(f"hi3861-flash: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())

"""Guest lifecycle and reconnectable consoles over a dedicated vport0.

The Unix socket is local to one named host, inside its private runtime
folder. Framing is versioned and checked; no request becomes a shell command.
"""
from __future__ import annotations

import contextlib
import fcntl
import json
import os
from pathlib import Path
import re
import secrets
import select
import socket
import struct
import sys
import termios
import time
import tomllib
import tty
import zlib

from a20_error import A20Error
from a20_make import REPO_ROOT
from a20_runtime import _alive, _path, _read

HEADER = struct.Struct("<4sHHQII")
VERSION = 2
MAX_PAYLOAD = 4096
HELLO, START, LIST, READ, WRITE, PAUSE, RESUME, STOP, DESTROY = range(1, 10)
STATS = 10
RESPONSE = 0x8000
RECORD = struct.Struct("<32sIIiIQQII")
STATES = {1: "running", 2: "paused", 3: "stopping", 4: "exited", 5: "pausing", 6: "draining"}
NAME = re.compile(r"[A-Za-z0-9][A-Za-z0-9_-]{0,30}\Z")


def _name(name: str) -> bytes:
    if not NAME.fullmatch(name):
        raise A20Error("guest names must use 1–31 letters, digits, '_' or '-'")
    return name.encode().ljust(32, b"\0")


def _frame(op: int, ident: int, data: bytes) -> bytes:
    if len(data) > MAX_PAYLOAD:
        raise A20Error("VM management payload exceeds 4096 bytes")
    prefix = struct.pack("<4sHHQI", b"A2VM", VERSION, op, ident, len(data))
    crc = zlib.crc32(data, zlib.crc32(prefix))
    raw = prefix + struct.pack("<I", crc) + data
    return b"\x7e" + raw.replace(b"\x7d", b"\x7d\x5d").replace(
        b"\x7e", b"\x7d\x5e") + b"\x7e"


def _receive(sock: socket.socket, op: int, ident: int) -> bytes:
    data = bytearray()
    escaped = discard = False
    deadline = time.monotonic() + 10
    while time.monotonic() < deadline:
        chunk = sock.recv(8192)
        if not chunk:
            raise OSError("VM manager connection closed")
        for byte in chunk:
            if byte == 0x7e:
                raw = bytes(data)
                valid = not escaped and not discard
                data.clear(); escaped = discard = False
                if not valid or len(raw) < HEADER.size:
                    continue
                magic, version, kind, got_id, size, crc = HEADER.unpack_from(raw)
                if (magic != b"A2VM" or version != VERSION or size > MAX_PAYLOAD or
                        len(raw) != HEADER.size + size or
                        zlib.crc32(raw[HEADER.size:], zlib.crc32(raw[:20])) != crc):
                    continue
                if kind != (op | RESPONSE) or got_id != ident:
                    continue
                payload = raw[HEADER.size:]
                if len(payload) < 4:
                    raise A20Error("VM manager returned a truncated response")
                status, = struct.unpack_from("<i", payload)
                if status < 0:
                    raise A20Error(f"VM manager: {os.strerror(-status)} (errno {-status})")
                return payload[4:]
            elif discard:
                continue
            elif escaped:
                data.append(byte ^ 0x20); escaped = False
            elif byte == 0x7d:
                escaped = True
            else:
                data.append(byte)
            if len(data) > HEADER.size + MAX_PAYLOAD:
                discard = True
    raise TimeoutError("VM manager response timed out")


def _rpc(host: str, op: int, payload: bytes = b"", *, expected_epoch: int | None = None) -> tuple[bytes, int]:
    path = _path(host)
    state = _read(path)
    if not _alive(state):
        raise A20Error(f"host {host} is not running")
    if not state.get("vm_manager"):
        raise A20Error(f"host {host} was not started with machine.vm_manager=true")
    ident = secrets.randbits(64) or 1
    deadline = time.monotonic() + 60
    failures = 0
    announced = False
    # Short connections multiplex independent consoles on port 0. Only HELLO
    # waits for initial boot; after readiness, at most two identical requests
    # are attempted and the manager epoch must remain unchanged.
    with (path / "manager.lock").open("a+") as lock:
        fcntl.flock(lock, fcntl.LOCK_EX)
        while True:
            try:
                with socket.socket(socket.AF_UNIX, socket.SOCK_STREAM) as sock:
                    sock.settimeout(3)
                    sock.connect(str(path / "control.sock"))
                    hello_id = secrets.randbits(64) or 1
                    sock.sendall(_frame(HELLO, hello_id, b""))
                    hello = _receive(sock, HELLO, hello_id)
                    if len(hello) != 20:
                        raise A20Error("VM manager HELLO layout mismatch")
                    version, max_guests, memory_mb, epoch = struct.unpack("<IIIQ", hello)
                    if version != VERSION:
                        raise A20Error("VM manager protocol version mismatch")
                    if expected_epoch is not None and epoch != expected_epoch:
                        raise A20Error("VM manager restarted during request; state must be queried again")
                    expected_epoch = epoch
                    if op == HELLO:
                        return hello, epoch
                    sock.sendall(_frame(op, ident, struct.pack("<Q", epoch) + payload))
                    return _receive(sock, op, ident), epoch
            except (OSError, TimeoutError) as exc:
                failures += 1
                if ((expected_epoch is not None and failures >= 2) or
                        time.monotonic() >= deadline or not _alive(state)):
                    raise A20Error(f"VM manager on {host} is unavailable: {exc}; "
                                   "check the host console for A20VMD: READY") from None
                if not announced and expected_epoch is None:
                    print(f"waiting for VM manager on {host}...", file=sys.stderr, flush=True)
                    announced = True
                time.sleep(0.05)


def _profile(value: str) -> dict:
    if "/" in value or value.endswith(".toml"):
        path = Path(value)
    else:
        new_path = REPO_ROOT / "instances" / "vm-profiles" / f"{value}.toml"
        old_path = REPO_ROOT / "vm-profiles" / f"{value}.toml"
        path = new_path if new_path.exists() else old_path
    try:
        with path.open("rb") as src:
            profile = tomllib.load(src)
    except (OSError, ValueError) as exc:
        raise A20Error(f"cannot read guest profile {path}: {exc}") from None
    if set(profile) != {"schema", "arch", "vmm", "accel", "vcpus", "memory_mb", "boot"}:
        raise A20Error("guest profile needs schema, arch, vmm, accel, vcpus, memory_mb and boot")
    if (type(profile["schema"]) is not int or profile["schema"] != 1 or
            profile["arch"] != "riscv64" or profile["vmm"] != "hypvm" or
            profile["accel"] != "native" or type(profile["vcpus"]) is not int or
            not 1 <= profile["vcpus"] <= 8):
        raise A20Error("current guest profile requires schema=1, riscv64, hypvm, native, vcpus=1..8")
    if type(profile["memory_mb"]) is not int or not 8 <= profile["memory_mb"] <= 256:
        raise A20Error("guest memory_mb must be 8–256")
    boot = profile["boot"]
    if not isinstance(boot, dict) or set(boot) != {"kernel"}:
        raise A20Error("guest boot must contain only kernel")
    if not isinstance(boot["kernel"], str) or not re.fullmatch(
            r"/bin/boot/[A-Za-z0-9_.-]+", boot["kernel"]) or ".." in boot["kernel"]:
        raise A20Error("guest kernel must be a bundled file under /bin/boot/")
    return profile


def list_guests(host: str) -> tuple[list[dict], int]:
    data, epoch = _rpc(host, LIST)
    if len(data) % RECORD.size:
        raise A20Error("VM manager returned a truncated guest list")
    records = []
    for offset in range(0, len(data), RECORD.size):
        name, pid, state, code, memory, generation, output_end, vcpus, reserved = RECORD.unpack_from(data, offset)
        records.append(dict(name=name.split(b"\0", 1)[0].decode("ascii"), pid=pid,
                            state=state, code=code, memory_mb=memory,
                            generation=generation, output_end=output_end, vcpus=vcpus))
    return records, epoch


def _write_cursor(path: Path, epoch: int, generation: int, cursor: int) -> None:
    temp = path.with_suffix(".tmp")
    temp.write_text(json.dumps(dict(epoch=epoch, generation=generation, cursor=cursor)) + "\n")
    temp.replace(path)


def console(host: str, name: str) -> None:
    encoded = _name(name)
    path = _path(host) / "guests"
    path.mkdir(mode=0o700, exist_ok=True)
    with (path / f"{name}.lock").open("a+") as lock:
        try:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
        except BlockingIOError:
            raise A20Error(f"guest {name} already has an attached console") from None
        records, epoch = list_guests(host)
        guest = next((g for g in records if g["name"] == name), None)
        if guest is None:
            raise A20Error(f"guest {name} does not exist")
        generation = guest["generation"]
        cursor_file = path / f"{name}.cursor"
        cursor = 0
        with contextlib.suppress(OSError, ValueError, TypeError, KeyError):
            saved = json.loads(cursor_file.read_text())
            if saved["epoch"] == epoch and saved["generation"] == generation:
                cursor = max(0, int(saved["cursor"]))
        input_fd = sys.stdin.fileno()
        old = termios.tcgetattr(input_fd) if os.isatty(input_fd) else None
        pending = bytearray()
        check_at = 0.0
        print(f"{host}/{name}: Ctrl-] detaches; guest keeps running.", flush=True)
        try:
            if old is not None:
                tty.setraw(input_fd)
            with (path / f"{name}.log").open("ab") as log:
                while True:
                    data, current_epoch = _rpc(host, READ, encoded + struct.pack("<QQI", generation, cursor, 2048),
                                               expected_epoch=epoch)
                    if current_epoch != epoch:
                        raise A20Error("VM manager restarted; reattach to query the new guest state")
                    if len(data) < 20:
                        raise A20Error("truncated guest console response")
                    start, next_cursor, gap = struct.unpack_from("<QQI", data)
                    output = data[20:]
                    if next_cursor - start != len(output) or start < cursor:
                        raise A20Error("invalid guest console cursor")
                    if gap:
                        sys.stderr.write("\r\n[older output exceeded retained history]\r\n")
                    if output:
                        sys.stdout.buffer.write(output)
                        sys.stdout.buffer.flush()
                        log.write(output); log.flush()
                        cursor = next_cursor
                        _write_cursor(cursor_file, epoch, generation, cursor)
                    if not output and time.monotonic() >= check_at:
                        records, current_epoch = list_guests(host)
                        current = next((g for g in records if g["name"] == name), None)
                        if (current_epoch != epoch or current is None or
                                current["generation"] != generation):
                            raise A20Error("guest identity changed; reattach to query its state")
                        if current["state"] == 4 and cursor >= current["output_end"]:
                            print(f"\r\n{name}: exited ({current['code']})", flush=True)
                            return
                        check_at = time.monotonic() + 0.5
                    if pending:
                        result, current_epoch = _rpc(host, WRITE, encoded + struct.pack("<Q", generation) + bytes(pending),
                                                      expected_epoch=epoch)
                        if current_epoch != epoch or len(result) != 4:
                            raise A20Error("guest input response identity mismatch")
                        accepted, = struct.unpack("<I", result)
                        if accepted > len(pending):
                            raise A20Error("invalid guest input acknowledgement")
                        del pending[:accepted]
                    ready, _, _ = select.select([input_fd] if len(pending) < 256 else [], [], [],
                                                 0 if output else 0.03)
                    if ready:
                        data = os.read(input_fd, 256 - len(pending))
                        if not data or b"\x1d" in data:
                            return
                        pending.extend(data)
        finally:
            if old is not None:
                termios.tcsetattr(input_fd, termios.TCSADRAIN, old)


def dispatch(args) -> None:
    action, host = args.vm_action, args.host
    if action == "start":
        name = args.name
        profile = _profile(args.profile)
        kernel = profile["boot"]["kernel"].encode()
        if len(kernel) >= 256:
            raise A20Error("guest kernel path exceeds 255 bytes")
        payload = _name(name) + struct.pack("<II", profile["memory_mb"], profile["vcpus"]) + kernel.ljust(256, b"\0")
        data, epoch = _rpc(host, START, payload)
        if len(data) != 8:
            raise A20Error("VM manager returned an invalid start result")
        generation, = struct.unpack("<Q", data)
        print(f"{host}/{name}: created (generation={generation}, manager={epoch}); "
              "use vm list to inspect boot/exit state")
    elif action == "list":
        records, _ = list_guests(host)
        for guest in records:
            print(f"{guest['name']}\t{STATES.get(guest['state'], 'unknown')}\t"
                  f"{guest['memory_mb']} MiB\tvcpus={guest['vcpus']}\tpid={guest['pid']}\texit={guest['code']}")
    elif action == "stats":
        data, _ = _rpc(host, STATS)
        if len(data) != 40: raise A20Error("VM manager stats layout mismatch")
        keys = ("vms", "vcpus", "mapped_pages", "table_pages", "free_host_pages")
        print(json.dumps(dict(zip(keys, struct.unpack("<5Q", data))), indent=2))
    elif action == "console":
        console(host, args.guest)
    else:
        op = {"pause": PAUSE, "resume": RESUME, "stop": STOP, "destroy": DESTROY}[action]
        records, epoch = list_guests(host)
        guest = next((g for g in records if g["name"] == args.guest), None)
        if guest is None and action != "destroy":
            raise A20Error(f"guest {args.guest} does not exist")
        generation = guest["generation"] if guest else 0
        _rpc(host, op, _name(args.guest) + struct.pack("<Q", generation), expected_epoch=epoch)
        print(f"{host}/{args.guest}: {action} accepted")

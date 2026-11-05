"""Bounded U-Boot handoff, with payload verification and one serial owner.

Only the documented RAM-loading commands are automated. Each preparatory
command must return a fresh, complete status line; its echoed input is not
success. The final boot is judged by the instance's kernel checks.
"""
from __future__ import annotations

import hashlib
import ipaddress
import json
import re
import secrets
import shlex
import zlib
from dataclasses import replace
from pathlib import Path

from a20_console import (ConsoleError, SessionResult, Transcript, _run_reset,
                         _seconds, run_console_session)

_ANSI = re.compile(r"\x1b\[[0-?]*[ -/]*[@-~]")
_PROMPT = re.compile(r"(?:^|\n)(?:=> |licheerv_nano# )$")
_ALLOWED = {"setenv", "fatload", "tftpboot", "fdt", "mmc", "usb", "virtio", "version", "bdinfo", "printenv"}
_ENV = {"loadaddr", "kernel_addr_r", "fdt_addr_r", "fdt_high", "serverip", "ipaddr", "autostart", "verify"}


def image_layout(metadata: dict) -> tuple[str, str, int, int]:
    """Return boot command, payload filename, transfer address and byte size."""
    protocol = metadata.get("boot_protocol", "booti")
    payload = metadata.get("payload_name", "kernel.bin")
    address = metadata.get("payload_load_address", metadata["load_address"])
    if protocol not in {"booti", "bootm"}:
        raise ValueError("unsupported kernel boot protocol")
    if protocol == "bootm":
        if metadata.get("board") != "lichee-pi-zero" or payload != "kernel.uimg":
            raise ValueError("bootm requires the Lichee Pi Zero uImage bundle")
    elif payload != "kernel.bin" or address != metadata["load_address"]:
        raise ValueError("booti must load kernel.bin at its ELF physical address")
    if not isinstance(address, str) or not re.fullmatch(r"0x[0-9a-fA-F]+", address):
        raise ValueError("invalid payload transfer address")
    if payload not in metadata["files"]:
        raise ValueError("bundle does not cover its boot payload")
    size = metadata["files"][payload]["size"]
    start = int(address, 16)
    if size <= 0 or start + size >= 1 << 64:
        raise ValueError("invalid payload transfer range")
    if protocol == "bootm":
        kernel = int(metadata["load_address"], 16)
        if (not 0x40000000 <= start < start + size <= 0x44000000
                or start < kernel + metadata["image_size"] and start + size > kernel):
            raise ValueError("uImage staging overlaps the kernel or exceeds V3s RAM")
    return protocol, payload, start, size


def clean_output(text: str) -> str:
    return _ANSI.sub("", text).replace("\r", "")


def load_plan(bundle: Path, board: str) -> tuple[list[str], dict]:
    """Check the exact artifacts that will be transferred before opening UART."""
    metadata = json.loads((bundle / "manifest.json").read_text())
    if not isinstance(metadata, dict):
        raise ValueError("bundle manifest must be a JSON object")
    if metadata.get("board") != board:
        raise ValueError("handoff bundle board does not match the instance")
    files = metadata.get("files", {})
    if not isinstance(files, dict):
        raise ValueError("bundle files must be a filename-to-record object")
    footprint = metadata.get("image_size")
    load_address = metadata.get("load_address")
    if (not isinstance(footprint, int) or isinstance(footprint, bool) or footprint <= 0
            or not isinstance(load_address, str) or not re.fullmatch(r"0x[0-9a-fA-F]+", load_address)
            or int(load_address, 16) + footprint >= 1 << 64):
        raise ValueError("bundle has an invalid Image memory range")
    for name, record in files.items():
        if (not isinstance(name, str) or not isinstance(record, dict)
                or type(record.get("size")) is not int or record["size"] < 0
                or not isinstance(record.get("sha256"), str)
                or not re.fullmatch(r"[0-9a-f]{64}", record["sha256"])):
            raise ValueError("bundle has an invalid file integrity record")
        if Path(name).name != name or name in {".", ".."}:
            raise ValueError("bundle filename is not a plain filename")
        data = (bundle / name).read_bytes()
        if len(data) != record["size"] or hashlib.sha256(data).hexdigest() != record["sha256"]:
            raise ValueError(f"bundle hash/size mismatch: {name}; package the instance again")
    if not {"kernel.bin", "kernel.elf", "uboot.cmd"}.issubset(files):
        raise ValueError("bundle manifest does not cover kernel and handoff script")
    protocol, payload, _, _ = image_layout(metadata)
    commands = [line.strip() for line in (bundle / "uboot.cmd").read_text().splitlines()
                if line.strip() and not line.lstrip().startswith("#")]
    if not commands or not commands[-1].startswith(protocol + " "):
        raise ValueError(f"automatic handoff requires {protocol} as its final command")
    for i, line in enumerate(commands):
        if any(char in line for char in ";|&`\n\r") or "$((" in line or "$(" in line:
            raise ValueError("automatic handoff accepts one plain command per line")
        words = shlex.split(line)
        if not words or len(line.encode()) > 340:
            raise ValueError("empty or overlong U-Boot command")
        if i == len(commands) - 1:
            if words not in ([protocol, "${loadaddr}", "-", "${fdt_addr_r}"],
                             [protocol, "${loadaddr}", "-", "${fdtcontroladdr}"]):
                raise ValueError("automatic handoff requires distinct kernel and FDT addresses")
        elif words[0] not in _ALLOWED:
            raise ValueError(f"command is outside the RAM handoff protocol: {words[0]}")
        if words[0] in {"mmc", "usb", "virtio"}:
            safe = {"mmc": {("rescan",), ("list",), ("dev", "0")},
                    "usb": {("start",), ("tree",), ("info",)},
                    "virtio": {("scan",), ("info",)}}
            if tuple(words[1:]) not in safe[words[0]]:
                raise ValueError(f"storage writes/unknown subcommands are outside the RAM handoff protocol: {line}")
        if words[0] in {"version", "bdinfo"} and len(words) != 1:
            raise ValueError("unexpected arguments to firmware inventory command")
        if words[0] == "setenv" and (len(words) != 3 or words[1] not in _ENV):
            raise ValueError("automatic handoff only sets documented volatile boot variables")
        if words[0] == "setenv" and ((words[1] == "autostart" and words[2] != "no")
                or (words[1] == "verify" and words[2] != "yes")):
            raise ValueError("handoff cannot enable autostart or disable payload verification")
        if words[0] == "setenv" and words[1] == "fdt_high" and words[2] not in {
                "0xffffffff", "0xffffffffffffffff"}:
            raise ValueError("handoff fdt_high may only retain the checked FDT in place")
        if words[0] in {"fatload", "tftpboot"}:
            if len(words) != (5 if words[0] == "fatload" else 3):
                raise ValueError("load command must specify one interface/device, destination and file")
            name = words[-1]
            dtb = {"pine-a64": "sun50i-a64-pine64.dtb",
                   "licheerv-nano": "sg2002-licheerv-nano-b.dtb",
                   "quartz64-zero": "rk3566-quartz64-zero.dtb",
                   "ox64": "bl808-pine64-ox64.dtb",
                   "lichee-pi-zero": "sun8i-v3s-licheepi-zero.dtb"}.get(board)
            if name not in files or name not in {payload, dtb}:
                raise ValueError(f"load has no verified local payload: {name}")
            expected_address = "${loadaddr}" if name == payload else "${fdt_addr_r}"
            if words[-2] != expected_address:
                raise ValueError(f"unexpected payload destination: {name}")
    return commands, metadata


class UBootSession:
    def __init__(self, transport, *, now, on_output=None):
        self.transport = transport
        self.now = now
        self.on_output = on_output
        self.transcript = Transcript()
        self.token = secrets.token_hex(6)
        self.sequence = 0

    def receive(self, timeout: float) -> str:
        data = self.transport.read(timeout)
        if data:
            self.transcript.append(data)
            if self.on_output:
                self.on_output(data.decode("utf-8", errors="replace"))
        return clean_output(self.transcript.text)

    def wait(self, pattern: re.Pattern, start: int, timeout: float):
        deadline = self.now() + timeout
        while True:
            text = self.receive(min(.1, max(0., deadline - self.now())))
            found = pattern.search(text[start:])
            if found:
                return found
            if self.now() >= deadline:
                raise ConsoleError(f"U-Boot response timeout ({timeout:g}s)")

    def prompt(self, timeout: float):
        deadline = self.now() + timeout
        sent_at = self.now() - 1
        while True:
            text = self.receive(min(.1, max(0., deadline - self.now())))
            if _PROMPT.search(text):
                return
            # Ctrl-C cancels the countdown; never send loader commands into an
            # unrecognized running OS. It cannot persist a boot environment.
            if self.now() - sent_at >= .5:
                self.transport.write(b"\x03\r")
                sent_at = self.now()
            if self.now() >= deadline:
                raise ConsoleError("U-Boot prompt not found; power-cycle with the console attached")

    def command(self, command: str, timeout: float) -> str:
        self.sequence += 1
        marker = f"A20_{self.token}_{self.sequence}"
        start = len(clean_output(self.transcript.text))
        line = f"if {command}; then echo {marker}_OK; else echo {marker}_FAIL; fi"
        self.transport.write(line.encode() + b"\r")
        result = self.wait(re.compile(rf"(?m)^{marker}_(OK|FAIL)$"), start, timeout)
        if result.group(1) != "OK":
            raise ConsoleError(f"U-Boot command failed: {command}")
        self.wait(_PROMPT, start, timeout)
        return clean_output(self.transcript.text)[start:]

    def value(self, variable: str, timeout: float) -> int:
        self.sequence += 1
        marker = f"A20_VALUE_{self.token}_{self.sequence}"
        text = self.command(f"echo {marker}=${{{variable}}}", timeout)
        found = re.search(rf"(?m)^{marker}=((?:0x)?[0-9a-fA-F]+)$", text)
        if not found:
            raise ConsoleError(f"U-Boot variable {variable} is missing or not hexadecimal")
        return int(found.group(1), 16)


def run_image_handoff(transport, inst, bundle: Path, *, server_ip=None,
                      board_ip=None, now=None, on_output=None) -> SessionResult:
    import time
    now = now or time.monotonic
    session = UBootSession(transport, now=now, on_output=on_output)
    stage = "handoff-bundle"
    try:
        commands, metadata = load_plan(bundle, inst.board)
        protocol, payload, transfer_address, transfer_size = image_layout(metadata)
        identity = {"name": inst.name,
                    "sha256": hashlib.sha256(inst.source.read_bytes()).hexdigest()}
        if metadata.get("instance") != identity:
            raise ConsoleError("handoff bundle is from another or modified instance; package this instance again")
        timeout = _seconds(inst.handoff.command_timeout, 30)
        prompt_timeout = _seconds(inst.handoff.prompt_timeout, 60)
        if inst.target.reset:
            _run_reset(inst.target.reset)
        stage = "handoff-prompt"
        session.prompt(prompt_timeout)
        stage = "handoff-shell"
        # An unknown vendor bootloader may use the simple parser instead of
        # hush. Prove the required shell grammar before transferring anything.
        session.command("test 1 = 1", timeout)
        session.command("setenv autostart no", timeout)
        session.command("setenv verify yes", timeout)
        for name, value in (("serverip", server_ip), ("ipaddr", board_ip)):
            if value:
                value = str(ipaddress.IPv4Address(value))
                session.command(f"setenv {name} {value}", timeout)
        if inst.handoff.method == "tftp" and not (server_ip and board_ip):
            raise ConsoleError("TFTP automation needs --server-ip and --board-ip")
        stage = "handoff-load"
        kernel_loaded = False
        for command in commands[:-1]:
            words = shlex.split(command)
            session.command(command, timeout)
            if words[0] in {"fatload", "tftpboot"}:
                name, dest = words[-1], words[-2]
                size = session.value("filesize", timeout)
                if size != metadata["files"][name]["size"]:
                    raise ConsoleError(f"loaded payload size mismatch: {name}")
                response = session.command(f"crc32 {dest} ${{filesize}}", timeout)
                matches = re.findall(r"==>\s*([0-9a-fA-F]{8})\b", response)
                expected = zlib.crc32((bundle / name).read_bytes()) & 0xffffffff
                if not matches or int(matches[-1], 16) != expected:
                    raise ConsoleError(f"loaded payload CRC32 mismatch: {name}")
                kernel_loaded |= name == payload
        if not kernel_loaded:
            raise ConsoleError(f"handoff did not load and verify {payload}")
        stage = "handoff-fdt"
        fdt_var = "fdtcontroladdr" if "${fdtcontroladdr}" in commands[-1] else "fdt_addr_r"
        fdt_address = session.value(fdt_var, timeout)
        kernel_address = session.value("loadaddr", timeout)
        load_address = int(metadata["load_address"], 16)
        if kernel_address != transfer_address:
            raise ConsoleError("loadaddr does not match the bundle's payload transfer address")
        # The DTB may not overlap even BSS that entry.S clears at handoff.
        if not fdt_address or load_address <= fdt_address < load_address + metadata["image_size"]:
            raise ConsoleError("FDT address overlaps the complete kernel memory footprint")
        session.command(f"fdt addr ${{{fdt_var}}}", timeout)
        header = session.command("fdt header", timeout)
        total = re.search(r"(?im)^\s*totalsize:\s*(0x[0-9a-f]+)", header)
        magic = re.search(r"(?im)^\s*magic:\s*0xd00dfeed\b", header)
        if not magic or not total or not 40 <= int(total.group(1), 16) <= 0x200000:
            raise ConsoleError("FDT header has no valid magic/total size")
        fdt_end = fdt_address + int(total.group(1), 16)
        if fdt_address < load_address + metadata["image_size"] and fdt_end > load_address:
            raise ConsoleError("FDT memory range overlaps the kernel Image/BSS")
        if fdt_address < transfer_address + transfer_size and fdt_end > transfer_address:
            raise ConsoleError("FDT memory range overlaps the boot payload")
        stage = "handoff-boot"
        transport.write(commands[-1].encode() + b"\r")
        # Preserve one serial connection; do not reset the board again after
        # handing it the kernel. Bootloader output is not a kernel pass.
        target = replace(inst.target, reset=None)
        def boot_output(text):
            # Keep bytes as they arrive, including a partial boot before a
            # local interrupt; run_console_session may not return its result.
            session.transcript.append(text.encode("utf-8"))
            if on_output:
                on_output(text)
        result = run_console_session(transport, replace(inst, target=target), now=now,
                                     on_output=boot_output)
        result.transcript = session.transcript
        return result
    except KeyboardInterrupt:
        return SessionResult(False, stage, ("operator interrupted automatic handoff",), (), session.transcript)
    except (ConsoleError, OSError, ValueError, KeyError, TypeError) as exc:
        return SessionResult(False, stage, (str(exc),), (), session.transcript)
    finally:
        transport.close()

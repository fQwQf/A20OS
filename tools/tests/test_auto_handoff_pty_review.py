"""Exercise automatic handoff through the production raw SerialTransport/PTY."""
from dataclasses import replace
import errno
import hashlib
import importlib.machinery
import importlib.util
import json
import os
from pathlib import Path
import pty
import re
import select
import sys
import tempfile
import threading
import time
from types import SimpleNamespace
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from a20_console import SerialTransport
from a20_handoff import run_image_handoff
from a20_instance import parse_instance


class CountedSerial(SerialTransport):
    closed_count = 0
    def close(self):
        self.closed_count += 1
        super().close()


class BoardPTY:
    """A minimal U-Boot peer; kernel success comes only after actual booti."""
    def __init__(self, *, bad_crc=False, bad_size=False, echo_only=False,
                 fdt=0x8f000000, kernel=True):
        self.bad_crc, self.bad_size, self.echo_only = bad_crc, bad_size, echo_only
        self.fdt, self.kernel = fdt, kernel
        self.master, slave = pty.openpty()
        self.serial = CountedSerial(os.ttyname(slave))
        os.close(slave)
        self.sent = []
        self.stopping = threading.Event()
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def reply(self, data):
        # Fragment real serial input across tokens and line boundaries.
        encoded = data.encode()
        for i in range(0, len(encoded), 17):
            if self.stopping.is_set(): return
            os.write(self.master, encoded[i:i + 17])

    def command(self, line):
        line = line.strip("\x03\r\n")
        if not line: return
        self.sent.append(line)
        self.reply(line + "\r\n")
        if line.startswith("booti "):
            if self.kernel:
                self.reply("[FDT] RAM range\r\nSystem ready (bringup, no userspace)\r\npart ok\r\nSystem is going down for power-off NOW\r\n")
            return
        status = re.search(r"then echo (A20_\w+)_OK;", line)
        if not status: return
        if self.echo_only:
            self.reply("licheerv_nano# ")
            return
        value = re.search(r"echo (A20_VALUE_\w+)=\$\{(\w+)\}", line)
        if value:
            values = {"filesize": 7 if self.bad_size else 6, "loadaddr": 0x80200000,
                      "fdtcontroladdr": self.fdt}
            self.reply(f"{value[1]}={values[value[2]]:x}\r\n")
        if "crc32 " in line:
            self.reply(f"CRC32 for 80200000 ... 80200005 ==> {(zlib.crc32(b'kernel') ^ int(self.bad_crc)):08x}\r\n")
        if "fdt header" in line:
            self.reply("magic: 0xd00dfeed\r\ntotalsize: 0x1000 (4096)\r\n")
        self.reply(status[1] + "_OK\r\nlicheerv_nano# ")

    def run(self):
        pending = b""
        try:
            self.reply("U-Boot production-PTY test\r\nlicheerv_nano# ")
            while not self.stopping.is_set():
                if not select.select([self.master], [], [], .05)[0]: continue
                data = os.read(self.master, 4096)
                if not data: break
                pending += data
                while b"\r" in pending:
                    line, pending = pending.split(b"\r", 1)
                    self.command(line.decode())
        except OSError as error:
            if error.errno not in (errno.EIO, errno.EBADF): raise

    def close(self):
        self.stopping.set()
        self.thread.join(2)
        self.serial.close()
        os.close(self.master)


class ProductionSerialHandoff(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.bundle = Path(self.tmp.name)
        self.inst = parse_instance(Path(__file__).resolve().parents[2] / "instances/licheerv-nano-e.toml")
        self.inst = replace(self.inst, target=replace(self.inst.target, boot_timeout="1s", boot_wait=0),
                            handoff=replace(self.inst.handoff, method="tftp", command_timeout="1s", prompt_timeout="1s"))
        for name, data in (("kernel.bin", b"kernel"), ("kernel.elf", b"elf-fixture"),
                           ("uboot.cmd", b"setenv loadaddr 0x80200000\ntftpboot ${loadaddr} kernel.bin\nbooti ${loadaddr} - ${fdtcontroladdr}\n")):
            (self.bundle / name).write_bytes(data)
        files = {p.name: {"size": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                 for p in self.bundle.iterdir()}
        (self.bundle / "manifest.json").write_text(json.dumps({"board": self.inst.board,
            "instance": {"name": self.inst.name, "sha256": hashlib.sha256(self.inst.source.read_bytes()).hexdigest()},
            "load_address": "0x80200000", "image_size": 0x1000000, "files": files}))

    def run_board(self, **options):
        board = BoardPTY(**options)
        try:
            result = run_image_handoff(board.serial, self.inst, self.bundle,
                                      server_ip="192.0.2.1", board_ip="192.0.2.2")
            self.assertGreaterEqual(board.serial.closed_count, 1)
            self.assertIn("U-Boot production-PTY test", result.transcript.text)
            return result, board.sent
        finally:
            board.close()

    def test_real_serial_success_and_fragmented_markers(self):
        result, sent = self.run_board()
        self.assertTrue(result.ok, result.missing)
        self.assertTrue(any(c.startswith("booti ") for c in sent))
        self.assertIn("part ok", result.transcript.text)

    def test_real_serial_echo_is_not_execution(self):
        result, sent = self.run_board(echo_only=True)
        self.assertFalse(result.ok)
        self.assertEqual(result.stage, "handoff-shell")
        self.assertFalse(any("tftpboot" in c for c in sent))

    def test_real_serial_bad_payload_or_overlapping_dtb_never_boots(self):
        for options in ({"bad_crc": True}, {"bad_size": True},
                        {"fdt": 0x80201000}, {"fdt": 0x801ffff0}):
            with self.subTest(options=options):
                result, sent = self.run_board(**options)
                self.assertFalse(result.ok)
                self.assertFalse(any(c.startswith("booti ") for c in sent))

    def test_real_serial_bootloader_success_is_not_kernel_success(self):
        result, sent = self.run_board(kernel=False)
        self.assertFalse(result.ok)
        self.assertEqual(result.stage, "console_check")

    def test_interrupted_prompt_preserves_transcript_and_closes(self):
        board = BoardPTY(echo_only=True)
        original_read = board.serial.read
        reads = 0
        def interrupted(timeout):
            nonlocal reads
            reads += 1
            if reads > 1:
                raise KeyboardInterrupt()
            return original_read(timeout)
        try:
            with patch.object(board.serial, "read", side_effect=interrupted):
                result = run_image_handoff(board.serial, self.inst, self.bundle)
            self.assertFalse(result.ok)
            self.assertIn("interrupted", result.missing[0])
            self.assertIn("U-Boot production-PTY test", result.transcript.text)
            self.assertGreaterEqual(board.serial.closed_count, 1)
        finally:
            board.close()

    def test_bad_json_is_a_closed_failure_result(self):
        (self.bundle / "manifest.json").write_text("null")
        board = BoardPTY()
        try:
            result = run_image_handoff(board.serial, self.inst, self.bundle)
            self.assertFalse(result.ok)
            self.assertEqual(result.stage, "handoff-bundle")
            self.assertIn("JSON object", result.missing[0])
            self.assertEqual(board.sent, [])
            self.assertGreaterEqual(board.serial.closed_count, 1)
        finally:
            board.close()

    def test_interrupted_kernel_phase_keeps_partial_boot_output(self):
        board = BoardPTY(kernel=False)
        original_read = board.serial.read
        kernel_reads = 0
        def interrupted(timeout):
            nonlocal kernel_reads
            if any(line.startswith("booti ") for line in board.sent):
                kernel_reads += 1
                if kernel_reads == 1:
                    return b"PARTIAL KERNEL BOOT EVIDENCE\n"
                raise KeyboardInterrupt()
            return original_read(timeout)
        try:
            with patch.object(board.serial, "read", side_effect=interrupted):
                result = run_image_handoff(board.serial, self.inst, self.bundle,
                    server_ip="192.0.2.1", board_ip="192.0.2.2")
            self.assertFalse(result.ok)
            self.assertEqual(result.stage, "handoff-boot")
            self.assertIn("PARTIAL KERNEL BOOT EVIDENCE", result.transcript.text)
            self.assertGreaterEqual(board.serial.closed_count, 1)
        finally:
            board.close()

    def test_m0sense_cli_opens_uart_before_one_shot_usb_reboot(self):
        loader = importlib.machinery.SourceFileLoader("a20_m0sense_pty_review",
            str(Path(__file__).resolve().parents[1] / "a20"))
        spec = importlib.util.spec_from_loader(loader.name, loader)
        module = importlib.util.module_from_spec(spec)
        loader.exec_module(module)
        inst = parse_instance(Path(__file__).resolve().parents[2] / "instances/m0sense-bl702-probe.toml")
        master, slave = pty.openpty()
        transport = None
        inst = replace(inst, target=replace(inst.target, boot_wait=0, boot_timeout="1s"))
        args = SimpleNamespace(auto_handoff=False, no_build=True, no_console=False,
            dry_run=False, uf2_mount=None, serial=os.ttyname(slave), make_args=[])
        log = self.bundle / "m0sense-one-shot.log"
        def open_serial(selected):
            nonlocal transport
            transport = CountedSerial(selected.target.serial)
            return transport
        def usb_upload(*args):
            self.assertIsNotNone(transport, "UART must already be open before the loader resets")
            os.write(master, b"A20 BL702 M-mode SRAM probe\r\nA20 BL702 PROBE READY\r\n")
        try:
            with patch.object(module, "open_transport", side_effect=open_serial), \
                 patch.object(module, "console_log_path", return_value=log), \
                 patch("m0sense_deploy_uf2.deploy", side_effect=usb_upload):
                self.assertEqual(module._deploy_m0sense(inst, args), 0)
            self.assertIn("A20 BL702 PROBE READY", log.read_text())
            self.assertGreaterEqual(transport.closed_count, 1)
        finally:
            if transport is not None: transport.close()
            os.close(master)
            os.close(slave)


if __name__ == "__main__":
    unittest.main()

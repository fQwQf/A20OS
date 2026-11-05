"""Protocol failures must stop automatic U-Boot handoff before a kernel jump."""
from pathlib import Path
from dataclasses import replace
import hashlib
import json
import re
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cli"))
from a20_handoff import load_plan, run_image_handoff
from a20_instance import parse_instance


class Clock:
    time = 0.
    def now(self):
        return self.time


class UBoot:
    def __init__(self, clock, *, failure=None, crc_bad=False, size_bad=False,
                 echo_only=False, kernel_log=True, prompt=True, fdt=0x8f000000,
                 kernel_address=0x80200000):
        self.clock, self.failure = clock, failure
        self.crc_bad, self.size_bad, self.echo_only = crc_bad, size_bad, echo_only
        self.kernel_log, self.fdt = kernel_log, fdt
        self.kernel_address = kernel_address
        self.queue = [b"U-Boot 2025.10\r\nlicheerv_nano# "] if prompt else []
        self.sent = []

    def read(self, timeout):
        self.clock.time += max(.01, timeout)
        return self.queue.pop(0) if self.queue else b""

    def write(self, data, timeout=10):
        text = data.decode().rstrip("\r")
        self.sent.append(text)
        if text.startswith(("booti ", "bootm ")):
            if self.kernel_log:
                self.queue.append(b"[FDT] RAM range\nSystem ready (bringup, no userspace)\npart ok\nSystem is going down for power-off NOW\n")
            return
        match = re.search(r"then echo (A20_\w+)_OK;", text)
        if not match:
            return
        marker = match.group(1)
        response = "licheerv_nano# " + text + "\r\n"
        if self.echo_only:
            self.queue.append((response + "licheerv_nano# ").encode())
            return
        value = re.search(r"echo (A20_VALUE_\w+)=\$\{(\w+)\}", text)
        if value:
            values = {"filesize": 7 if self.size_bad else 6, "loadaddr": self.kernel_address,
                      "fdtcontroladdr": self.fdt, "fdt_addr_r": self.fdt}
            response += f"{value.group(1)}={values[value.group(2)]:x}\r\n"
        if "crc32 " in text:
            crc = zlib.crc32(b"kernel") ^ int(self.crc_bad)
            response += f"CRC32 for 80200000 ... 80200005 ==> {crc:08x}\r\n"
        if "fdt header" in text:
            response += "magic: 0xd00dfeed\r\ntotalsize: 0x1000 (4096)\r\n"
        failed = self.failure and self.failure in text
        response += marker + ("_FAIL" if failed else "_OK") + "\r\nlicheerv_nano# "
        self.queue.append(response.encode())

    def close(self):
        self.closed = True


class AutoHandoff(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.bundle = Path(self.temp.name)
        self.inst = parse_instance(Path(__file__).resolve().parents[2] / "instances/licheerv-nano-e.toml")
        self.inst = replace(self.inst, target=replace(self.inst.target, boot_timeout="1s"),
            handoff=replace(self.inst.handoff, command_timeout="1s", prompt_timeout="1s"))
        (self.bundle / "kernel.bin").write_bytes(b"kernel")
        (self.bundle / "kernel.elf").write_bytes(b"elf-fixture")
        (self.bundle / "uboot.cmd").write_text(
            "setenv loadaddr 0x80200000\ntftpboot ${loadaddr} kernel.bin\n"
            "booti ${loadaddr} - ${fdtcontroladdr}\n")
        self.record()

    def record(self):
        files = {p.name: {"size": p.stat().st_size, "sha256": hashlib.sha256(p.read_bytes()).hexdigest()}
                 for p in self.bundle.iterdir() if p.name != "manifest.json"}
        (self.bundle / "manifest.json").write_text(json.dumps({"board": "licheerv-nano",
            "instance": {"name": self.inst.name,
                         "sha256": hashlib.sha256(self.inst.source.read_bytes()).hexdigest()},
            "load_address": "0x80200000", "image_size": 0x1000000, "files": files}))

    def run_board(self, **options):
        clock = Clock()
        board = UBoot(clock, **options)
        result = run_image_handoff(board, self.inst, self.bundle, server_ip="192.0.2.1",
            board_ip="192.0.2.2", now=clock.now)
        self.assertTrue(board.closed)
        return result, board

    def test_success_verifies_size_crc_and_fdt_before_boot(self):
        result, board = self.run_board()
        self.assertTrue(result.ok, result.missing)
        boot = next(i for i, line in enumerate(board.sent) if line.startswith("booti "))
        self.assertTrue(any("crc32 " in line for line in board.sent[:boot]))
        self.assertTrue(any("fdt header" in line for line in board.sent[:boot]))
        self.assertIn("U-Boot 2025.10", result.transcript.text)
        self.assertIn("part ok", result.transcript.text)

    def test_failed_load_size_crc_or_fdt_never_boots(self):
        for options in ({"failure": "tftpboot"}, {"size_bad": True}, {"crc_bad": True},
                        {"fdt": 0x80200000}, {"fdt": 0x801ffff0}):
            with self.subTest(options=options):
                result, board = self.run_board(**options)
                self.assertFalse(result.ok)
                self.assertFalse(any(line.startswith("booti ") for line in board.sent))

    def test_command_echo_is_not_a_status_line(self):
        result, board = self.run_board(echo_only=True)
        self.assertFalse(result.ok)
        self.assertEqual(result.stage, "handoff-shell")
        self.assertFalse(any("tftpboot" in line for line in board.sent))

    def test_missing_prompt_does_not_send_loader_commands(self):
        result, board = self.run_board(prompt=False)
        self.assertFalse(result.ok)
        self.assertEqual(result.stage, "handoff-prompt")
        self.assertTrue(all(line == "\x03" for line in board.sent))

    def test_bootloader_success_does_not_prove_kernel_boot(self):
        result, board = self.run_board(kernel_log=False)
        self.assertFalse(result.ok)
        self.assertEqual(result.stage, "console_check")

    def test_corrupt_bundle_stops_before_touching_serial(self):
        (self.bundle / "kernel.bin").write_bytes(b"changed")
        result, board = self.run_board()
        self.assertFalse(result.ok)
        self.assertEqual(result.stage, "handoff-bundle")
        self.assertEqual(board.sent, [])

    def test_persistent_commands_are_rejected(self):
        recipe = self.bundle / "uboot.cmd"
        original = recipe.read_text()
        for line in ("saveenv", "mmc erase 0 0x1000", "mmc write 80200000 0 10", "usb write 80200000 0 10"):
            with self.subTest(command=line):
                recipe.write_text(line + "\n" + original)
                self.record()
                with self.assertRaisesRegex(ValueError, "outside the RAM"):
                    load_plan(self.bundle, "licheerv-nano")

    def test_actual_nano_mmc_recipe_accepts_its_manifest_verified_dtb(self):
        # Use the repository recipe verbatim: a synthetic control-DTB-only
        # recipe cannot catch omissions from the real board DTB allowlist.
        self.inst = parse_instance(Path(__file__).resolve().parents[2] / "instances/licheerv-nano.toml")
        (self.bundle / "sg2002-licheerv-nano-b.dtb").write_bytes(b"nano-dtb-fixture")
        recipe = "setenv loadaddr 0x80200000\n" + "\n".join(self.inst.handoff.commands) + "\n"
        (self.bundle / "uboot.cmd").write_text(recipe)
        self.record()
        commands, metadata = load_plan(self.bundle, self.inst.board)
        self.assertIn("fatload mmc 0:1 ${fdt_addr_r} sg2002-licheerv-nano-b.dtb", commands)
        self.assertEqual(commands[-1], "booti ${loadaddr} - ${fdt_addr_r}")
        self.assertIn("sg2002-licheerv-nano-b.dtb", metadata["files"])

    def bootm_fixture(self, address="0x42000000"):
        self.inst = replace(self.inst, board="lichee-pi-zero")
        (self.bundle / "kernel.uimg").write_bytes(b"kernel")
        (self.bundle / "uboot.cmd").write_text(
            f"setenv loadaddr {address}\nsetenv fdt_addr_r 0x41800000\n"
            "fatload mmc 0:1 ${loadaddr} kernel.uimg\n"
            "bootm ${loadaddr} - ${fdt_addr_r}\n")
        self.record()
        path = self.bundle / "manifest.json"
        metadata = json.loads(path.read_text())
        metadata.update(board="lichee-pi-zero", boot_protocol="bootm",
                        payload_name="kernel.uimg", payload_load_address=address,
                        load_address="0x40080000", image_size=0x1000000)
        path.write_text(json.dumps(metadata))

    def test_bootm_verifies_staged_uimage_before_unpacking_kernel(self):
        self.bootm_fixture()
        result, board = self.run_board(kernel_address=0x42000000, fdt=0x41800000)
        self.assertTrue(result.ok, result.missing)
        self.assertIn("bootm ${loadaddr} - ${fdt_addr_r}", board.sent)
        self.assertTrue(any("crc32 ${loadaddr}" in line for line in board.sent))

    def test_bootm_rejects_overlapping_staging_and_fdt(self):
        self.bootm_fixture("0x40080000")
        result, board = self.run_board()
        self.assertFalse(result.ok)
        self.assertEqual(board.sent, [])
        self.bootm_fixture()
        for fdt in (0x40090000, 0x42000000, 0x41fffff0):
            with self.subTest(fdt=fdt):
                result, board = self.run_board(kernel_address=0x42000000, fdt=fdt)
                self.assertFalse(result.ok)
                self.assertFalse(any(c.startswith("bootm ") for c in board.sent))

    def test_board_recipes_accept_their_actual_payload_and_dtb_names(self):
        for board, load, dtb in (
                ("lichee-pi-zero", "0x40080000", "sun8i-v3s-licheepi-zero.dtb"),
                ("quartz64-zero", "0x02080000", "rk3566-quartz64-zero.dtb"),
                ("ox64", "0x50200000", "bl808-pine64-ox64.dtb")):
            with self.subTest(board=board):
                self.inst = parse_instance(Path(__file__).resolve().parents[2] / "instances" / (board + ".toml"))
                (self.bundle / dtb).write_bytes(b"board-dtb-fixture")
                (self.bundle / "kernel.uimg").write_bytes(b"kernel")
                recipe = f"setenv loadaddr {load}\n" + "\n".join(self.inst.handoff.commands) + "\n"
                (self.bundle / "uboot.cmd").write_text(recipe)
                self.record()
                path = self.bundle / "manifest.json"
                metadata = json.loads(path.read_text())
                metadata.update(board=board, load_address=load)
                if board == "lichee-pi-zero":
                    metadata.update(boot_protocol="bootm", payload_name="kernel.uimg",
                                    payload_load_address="0x42000000")
                path.write_text(json.dumps(metadata))
                commands, _ = load_plan(self.bundle, board)
                self.assertIn(dtb, "\n".join(commands))


if __name__ == "__main__":
    unittest.main()

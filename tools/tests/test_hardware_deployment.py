"""Hardware-free regressions for destructive-write guards and boot packaging."""
from __future__ import annotations

import importlib.machinery
import importlib.util
import io
import json
import os
import pty
import struct
import sys
import tempfile
import termios
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

TOOLS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(TOOLS))
import kernel_bundle
import media
from a20_instance import parse_instance
from a20_make import build_instance
from a20_board import run_package
from a20_console import InteractiveTransport, ConsoleError


def a20_module():
    loader = importlib.machinery.SourceFileLoader("a20_hardware_test", str(TOOLS / "a20"))
    spec = importlib.util.spec_from_loader(loader.name, loader)
    module = importlib.util.module_from_spec(spec)
    loader.exec_module(module)
    return module


class MediaMounts(unittest.TestCase):
    def lsblk(self, nodes, status=0):
        return SimpleNamespace(returncode=status, stdout=json.dumps({"blockdevices": nodes}), stderr="failed")

    def check(self, reply):
        with patch.object(media.shutil, "which", return_value="/usr/bin/lsblk"), \
                patch.object(media.subprocess, "run", return_value=reply), \
                patch.object(Path, "read_text", return_value="Filename Type Size Used Priority\n"):
            return media.is_mounted("/dev/sdz")

    def test_mounted_child_blocks_whole_disk(self):
        self.assertTrue(self.check(self.lsblk([{"path": "/dev/sdz", "mountpoints": [None],
            "children": [{"path": "/dev/sdz1", "mountpoints": ["/"]}]}])))

    def test_unmounted_disk_can_be_inspected(self):
        self.assertFalse(self.check(self.lsblk([{"path": "/dev/sdz", "mountpoints": [None],
            "children": [{"path": "/dev/sdz1", "mountpoints": []}]}])))

    def test_inspection_failure_does_not_authorize_write(self):
        with self.assertRaises(ValueError):
            self.check(self.lsblk([], status=1))
        with self.assertRaises(ValueError):
            self.check(self.lsblk([]))

    def test_swap_child_blocks_whole_disk(self):
        reply = self.lsblk([{"path": "/dev/sdz", "mountpoints": [],
            "children": [{"path": "/dev/sdz2", "mountpoints": []}]}])
        with patch.object(media.shutil, "which", return_value="lsblk"), \
                patch.object(media.subprocess, "run", return_value=reply), \
                patch.object(Path, "read_text", return_value="Filename Type\n/dev/sdz2 partition\n"):
            self.assertTrue(media.is_mounted("/dev/sdz"))

    def test_oversize_and_multiple_images_never_call_dd(self):
        with tempfile.TemporaryDirectory() as tmp:
            image = Path(tmp) / "kernel.img"
            image.write_bytes(b"x" * 16)
            for paths in (str(image), f"{image} {image}"):
                with patch.object(sys, "argv", ["media.py", "--boot-media", paths, "--media-device", "/dev/sdz"]), \
                        patch.object(media, "check_block_device", return_value=None), \
                        patch.object(media, "is_mounted", return_value=False), \
                        patch.object(media, "device_capacity", return_value=8), \
                        patch.object(media, "write") as write:
                    self.assertEqual(media.main(), 1)
                    write.assert_not_called()


class BootBundle(unittest.TestCase):
    def fixture(self, tmp, machine=183, valid=True):
        elf = bytearray(120)
        elf[:6] = b"\x7fELF\x02\x01"
        struct.pack_into("<H", elf, 18, machine)
        struct.pack_into("<Q", elf, 32, 64)
        struct.pack_into("<HH", elf, 54, 56, 1)
        struct.pack_into("<I", elf, 64, 1)
        struct.pack_into("<QQQ", elf, 88, 0x40080000, 64, 4096)
        image = bytearray(64)
        struct.pack_into("<Q", image, 16, 4096)
        if valid:
            if machine == 183:
                image[56:60] = b"ARM\x64"
            else:
                image[48:56] = b"RISCV\x00\x00\x00"
                image[56:60] = b"RSC\x05"
        ep, kp = Path(tmp) / "kernel.elf", Path(tmp) / "kernel.bin"
        ep.write_bytes(elf)
        kp.write_bytes(image)
        return ep, kp

    def test_both_image_protocols_and_bss_footprint(self):
        with tempfile.TemporaryDirectory() as tmp:
            for machine in (183, 243):
                self.assertEqual(kernel_bundle.image_info(*self.fixture(tmp, machine)), (0x40080000, 4096))

    def test_raw_binary_is_not_booti_image(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaisesRegex(ValueError, "magic"):
                kernel_bundle.image_info(*self.fixture(tmp, valid=False))

    def test_image_header_must_cover_bss(self):
        with tempfile.TemporaryDirectory() as tmp:
            ep, kp = self.fixture(tmp)
            image = bytearray(kp.read_bytes())
            struct.pack_into("<Q", image, 16, 64)
            kp.write_bytes(image)
            with self.assertRaisesRegex(ValueError, "including BSS"):
                kernel_bundle.image_info(ep, kp)

    def test_commands_come_from_instance_and_dtb_is_distinct(self):
        with tempfile.TemporaryDirectory() as tmp, redirect_stdout(io.StringIO()):
            ep, kp = self.fixture(tmp)
            manifest = Path(tmp) / "board.toml"
            manifest.write_text('board="test-board"\n[handoff]\ncommands=["fatload mmc 0:1 ${loadaddr} kernel.bin", "booti ${loadaddr} - ${fdt_addr_r}"]\n')
            out = Path(tmp) / "handoff"
            kernel_bundle.package(ep, kp, out, "test-board", manifest)
            recipe = (out / "uboot.cmd").read_text()
            self.assertIn("setenv loadaddr 0x40080000", recipe)
            self.assertIn("booti ${loadaddr} - ${fdt_addr_r}", recipe)
            self.assertNotIn("- ${loadaddr}", recipe)
            data = json.loads((out / "manifest.json").read_text())
            self.assertFalse(data["hardware_validated"])
            self.assertEqual(data["image_size"], 4096)


class DeploymentPhases(unittest.TestCase):
    def test_physical_board_does_not_launch_an_unrelated_qemu_virt(self):
        from a20_error import A20Error
        from a20_instance import applicable_actions
        module = a20_module()
        for name in ("lichee-pi-zero", "quartz64-zero", "ox64"):
            with self.subTest(board=name):
                inst = parse_instance(TOOLS.parent / "instances" / (name + ".toml"))
                self.assertNotIn("run", applicable_actions(inst))
                self.assertNotIn("debug", applicable_actions(inst))
                with self.assertRaises(A20Error):
                    module._check_runnable(inst, "run")

    def test_ls2k_stages_bundle_before_returning_without_serial(self):
        module = a20_module()
        args = SimpleNamespace(instance="ls2k0300", stage_mount="/media/FAT",
            no_flash=False, no_build=True, no_console=True, dry_run=False,
            make_args=[], media_device=None)
        with patch("ls2k0300_handoff.stage_bundle", return_value={
                "destination": "/media/FAT/a20/kernel.elf", "sha256": "abc"}) as stage, \
                patch.object(module, "cmd_console") as console, redirect_stdout(io.StringIO()):
            self.assertEqual(module.cmd_deploy(args), 0)
            self.assertEqual(stage.call_args.args[1], Path("/media/FAT"))
            console.assert_not_called()

    def test_stage_mount_rejects_other_boards_before_writes(self):
        module = a20_module()
        from a20_error import A20Error
        with patch.object(module, "run_package") as package:
            with self.assertRaises(A20Error):
                module.cmd_deploy(SimpleNamespace(instance="pine-a64", stage_mount="/media/FAT"))
            package.assert_not_called()

    def test_interactive_input_uses_same_transport_and_restores_terminal(self):
        class Serial:
            sent = b""
            closed = False
            def read(self, timeout):
                return b"U-Boot> "
            def write(self, data, timeout=10):
                self.sent += data
            def close(self):
                self.closed = True
        master, slave = pty.openpty()
        original = termios.tcgetattr(slave)
        serial = Serial()
        try:
            interactive = InteractiveTransport(serial, slave)
            os.write(master, b"version\n")
            self.assertEqual(interactive.read(0), b"U-Boot> ")
            self.assertEqual(serial.sent, b"version\n")
            self.assertEqual(interactive.transcript.text, "U-Boot> ")
            os.write(master, b"\x1d")
            with self.assertRaises(ConsoleError):
                interactive.read(0)
            interactive.close()
            self.assertTrue(serial.closed)
            self.assertEqual(termios.tcgetattr(slave), original)
        finally:
            os.close(master)
            os.close(slave)

    def test_prepare_only_does_not_open_serial_and_override_reaches_writer(self):
        module = a20_module()
        args = SimpleNamespace(instance="j1900-pc", no_flash=False, no_reset=False,
            no_build=True, no_console=True, dry_run=False, make_args=[],
            media_device="/dev/disk/by-id/usb-TEST", serial="/dev/ttyUSB9")
        with patch.object(module, "exec_make") as make, \
                patch.object(module, "run_package") as package, \
                patch.object(module, "cmd_console") as console, redirect_stdout(io.StringIO()):
            self.assertEqual(module.cmd_deploy(args), 0)
            package.assert_not_called()
            console.assert_not_called()
            self.assertEqual(make.call_args.args[0].target.media_device, args.media_device)
            self.assertEqual(make.call_args.args[1], "target-write-media")

    def test_mcu_probe_does_not_build_a_supervisor_kernel(self):
        path = TOOLS.parent / "instances/m0sense-bl702-probe.toml"
        if not path.is_file():
            self.skipTest("MCU instance is being introduced")
        inst = parse_instance(path)
        with patch("a20_make.exec_make") as make:
            build_instance(inst, [], True)
            self.assertEqual(make.call_args.args[1], "m0sense-probe")
        with patch("a20_board.exec_make") as make:
            run_package(inst, [], True)
            self.assertEqual(make.call_args.args[1], "m0sense-probe")


if __name__ == "__main__":
    unittest.main()

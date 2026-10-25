"""SG2002 FAT/FIP contracts. No network or physical devices are used."""
from pathlib import Path
import json
import shutil
import struct
import subprocess
import sys
import tempfile
from types import SimpleNamespace
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
import licheerv_nano as nano
import pine_a64 as common
from raspberrypi4_fixture import Tree


def tree(memory=0x10000000):
    t = Tree()
    t.begin("")
    t.text("compatible", "sipeed,licheerv-nano-b\0sipeed,licheerv-nano\0sophgo,sg2002")
    t.cells("#address-cells", 1)
    t.cells("#size-cells", 1)
    t.begin("memory@80000000")
    t.cells("reg", 0x80000000, memory)
    t.end()
    t.end()
    return t.finish([])


def fip(rtos=0):
    data = bytearray(8192)
    data[:8] = b"CVBL01\n\0"
    struct.pack_into("<I", data, 0xE0, 4096)
    data[4096:4104] = b"CVLD02\n\0"
    struct.pack_into("<I", data, 4096 + 40, rtos)
    struct.pack_into("<I", data, 4096 + 60, 0x80000000)
    return data


class NanoDeployTests(unittest.TestCase):
    def test_environment_buffer_is_outside_image_and_dtb(self):
        with tempfile.TemporaryDirectory() as directory:
            header = Path(directory) / "config.h"
            header.write_text("scriptaddr=0x80c00000\nkernel_addr_r=0x81000000\n")
            nano.patch_environment(header)
            self.assertIn("scriptaddr=0x8e000000", header.read_text())
            self.assertIn("kernel_addr_r=0x80200000", header.read_text())
            with self.assertRaises(common.PrepareError):
                nano.patch_environment(header)
        self.assertGreater(nano.SCRIPT_ADDRESS, nano.FDT_ADDRESS + 0x200000)

    def test_dtb_memory_tuple(self):
        self.assertEqual(nano.dtb_memory(tree()), (0x80000000, 0x10000000))
        self.assertEqual(nano.dtb_memory(tree(0x4000000)), (0x80000000, 0x4000000))
        with self.assertRaises(common.PrepareError):
            nano.dtb_memory(b"invalid")

    def test_optional_secondary_rtos_must_be_absent(self):
        nano.validate_fip(fip())
        with self.assertRaises(common.PrepareError):
            nano.validate_fip(fip(rtos=512))
        damaged = fip()
        struct.pack_into("<I", damaged, 0xE0, 8192)
        with self.assertRaises(common.PrepareError):
            nano.validate_fip(damaged)

    def test_both_files_are_size_and_crc_checked(self):
        kernel, dtb = b"nano kernel", tree()
        text = nano.boot_script(kernel, dtb)
        self.assertIn(f"itest ${{filesize}} == 0x{len(kernel):x}", text)
        self.assertIn(f"itest ${{filesize}} == 0x{len(dtb):x}", text)
        self.assertIn(f"{zlib.crc32(kernel):08x}", text)
        self.assertIn(f"{zlib.crc32(dtb):08x}", text)
        self.assertIn("booti ${loadaddr} - ${fdt_addr_r}", text)
        self.assertNotIn("saveenv", text)

    def test_actual_firmware_image_roundtrip_and_footprint_rejection(self):
        firmware = ROOT / "build/licheerv-nano/firmware"
        if not ((firmware / "manifest.json").is_file() and shutil.which("mcopy")):
            self.skipTest("run Nano prepare and install mtools for the real-image integration")
        nano.validate_firmware(firmware)
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            bundle = root / "handoff"
            bundle.mkdir()
            kernel = bytearray(128)
            kernel[48:56] = b"RISCV\0\0\0"
            kernel[56:60] = b"RSC\x05"
            struct.pack_into("<QQ", kernel, 8, 0x200000, 4096)
            (bundle / "kernel.bin").write_bytes(kernel)
            meta = {"board": "licheerv-nano", "load_address": "0x80200000",
                    "files": {"kernel.bin": common.artifact(bundle / "kernel.bin")}}
            (bundle / "manifest.json").write_text(json.dumps(meta))
            output = root / "nano.img"
            args = SimpleNamespace(firmware=firmware, bundle=bundle, output=output, size_mib=128)
            nano.sdcard(args)
            with output.open("rb") as stream:
                self.assertEqual(stream.read(512), common.mbr(output.stat().st_size))
                stream.seek(8192)
                self.assertEqual(stream.read(4096), bytes(4096))  # No Allwinner raw SPL.
            for name in ("fip.bin", nano.DTB_NAME):
                actual = subprocess.run(["mcopy", "-i", f"{output}@@8388608", "::/" + name, "-"],
                                        check=True, capture_output=True).stdout
                self.assertEqual(actual, (firmware / name).read_bytes())
            script = subprocess.run(["mcopy", "-i", f"{output}@@8388608", "::/boot.scr", "-"],
                                    check=True, capture_output=True).stdout
            self.assertEqual(script[:4], b"\x27\x05\x19\x56")
            struct.pack_into("<Q", kernel, 16, 0x2000000)
            (bundle / "kernel.bin").write_bytes(kernel)
            meta["files"]["kernel.bin"] = common.artifact(bundle / "kernel.bin")
            (bundle / "manifest.json").write_text(json.dumps(meta))
            with self.assertRaises(common.PrepareError):
                nano.sdcard(args)


if __name__ == "__main__":
    unittest.main()

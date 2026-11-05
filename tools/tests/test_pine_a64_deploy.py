"""Pinned firmware/whole-card layout contracts; no hardware writes or downloads."""
from pathlib import Path
import json
import shutil
import struct
import subprocess
import sys
import tarfile
import tempfile
from types import SimpleNamespace
import unittest
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "boards"))
import pine_a64 as pine
from raspberrypi4_fixture import Tree


def dtb(compatible="pine64,pine64"):
    tree = Tree()
    tree.begin("")
    tree.text("compatible", compatible + "\0allwinner,sun50i-a64")
    tree.end()
    return tree.finish([])


class PineImageContracts(unittest.TestCase):
    def test_source_archive_digest_mismatch_is_rejected_without_network(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            spec = pine.SOURCES["u-boot"]
            (cache / f"u-boot-{spec['commit'][:12]}.tar.gz").write_bytes(b"modified")
            with self.assertRaises(pine.PrepareError):
                pine.source_tree("u-boot", cache)

    def test_custom_source_spec_reextracts_verified_archive(self):
        with tempfile.TemporaryDirectory() as directory:
            cache = Path(directory)
            upstream = cache / "original"
            upstream.mkdir()
            (upstream / "README").write_text("pinned contents")
            commit = "123456789abc" + "0" * 28
            archive = cache / "fixture-123456789abc.tar.gz"
            with tarfile.open(archive, "w:gz") as packed:
                packed.add(upstream, arcname="upstream")
            spec = {"commit": commit, "sha256": pine.sha256(archive)}
            result = pine.source_tree("fixture", cache, spec)
            (result / "README").write_text("local modification")
            result = pine.source_tree("fixture", cache, spec)
            self.assertEqual((result / "README").read_text(), "pinned contents")

    def test_mbr_bootable_fat32_and_firmware_gap(self):
        data = pine.mbr(128 * 1024 * 1024)
        status, _, kind, _, start, count = struct.unpack_from("<B3sB3sII", data, 446)
        self.assertEqual((status, kind, start), (0x80, 0x0C, 16384))
        self.assertEqual((start + count) * 512, 128 * 1024 * 1024)
        self.assertEqual(data[510:], b"\x55\xaa")
        self.assertGreater(pine.PARTITION_OFFSET, pine.FIRMWARE_OFFSET)

    def test_too_small_or_unaligned_image_is_rejected(self):
        for size in (0, 64 * 1024 * 1024, 128 * 1024 * 1024 + 1):
            with self.assertRaises(pine.PrepareError):
                pine.mbr(size)

    def test_original_and_plus_dtb_are_distinguishable(self):
        self.assertIn("pine64,pine64", pine.root_compatible(dtb()))
        self.assertNotIn("pine64,pine64", pine.root_compatible(dtb("pine64,pine64-plus")))
        with self.assertRaises(pine.PrepareError):
            pine.root_compatible(b"not a DTB")

    def test_output_refuses_devices_and_symlinks(self):
        with self.assertRaises(pine.PrepareError):
            pine.regular_output(Path("/dev/null"))
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "image"
            path.symlink_to("missing-target")
            with self.assertRaises(pine.PrepareError):
                pine.regular_output(path)

    def test_script_checks_both_payload_sizes_and_crcs_before_booti(self):
        kernel, tree = b"known kernel", dtb()
        text = pine.boot_script(kernel, tree)
        self.assertIn(f"itest ${{filesize}} == 0x{len(kernel):x}", text)
        self.assertIn(f"itest ${{filesize}} == 0x{len(tree):x}", text)
        self.assertIn(f"crc32 -v ${{loadaddr}} ${{filesize}} {zlib.crc32(kernel):08x}", text)
        self.assertIn(f"crc32 -v ${{fdt_addr_r}} ${{filesize}} {zlib.crc32(tree):08x}", text)
        self.assertIn("booti ${loadaddr} - ${fdt_addr_r}", text)
        self.assertNotIn("saveenv", text)

    def test_whole_image_can_be_read_back_with_mtools(self):
        mkimage = shutil.which("mkimage")
        prepared_mkimage = ROOT / "build/pine-a64/firmware/mkimage"
        if not mkimage and prepared_mkimage.is_file():
            mkimage = str(prepared_mkimage)
        if not (mkimage and shutil.which("mcopy") and
                (shutil.which("mkfs.fat") or Path("/usr/sbin/mkfs.fat").exists())):
            self.skipTest("integration needs mkimage, dosfstools and mtools")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            firmware, bundle = root / "firmware", root / "handoff"
            firmware.mkdir()
            bundle.mkdir()
            spl = b"\x00" * 4 + b"eGON.BT0" + b"\x00" * 52
            (firmware / "u-boot-sunxi-with-spl.bin").write_bytes(spl)
            (firmware / pine.DTB_NAME).write_bytes(dtb())
            shutil.copy2(mkimage, firmware / "mkimage")
            fw = {"board": "pine-a64", "ram_mib": 512,
                  "config_overrides": ["CONFIG_CRC32_VERIFY=y"],
                  "files": {name: pine.artifact(firmware / name) for name in
                            ("u-boot-sunxi-with-spl.bin", pine.DTB_NAME, "mkimage")}}
            (firmware / "manifest.json").write_text(json.dumps(fw))
            image_header = bytearray(128)
            struct.pack_into("<QQ", image_header, 8, 0x80000, 4096)
            image_header[56:60] = b"ARMd"
            (bundle / "kernel.bin").write_bytes(image_header)
            meta = {"board": "pine-a64", "load_address": "0x40080000",
                    "files": {"kernel.bin": pine.artifact(bundle / "kernel.bin")}}
            (bundle / "manifest.json").write_text(json.dumps(meta))
            output = root / "image.img"
            pine.sdcard(SimpleNamespace(firmware=firmware, bundle=bundle,
                                       output=output, size_mib=128))
            with output.open("rb") as stream:
                self.assertEqual(stream.read(512), pine.mbr(output.stat().st_size))
                stream.seek(pine.FIRMWARE_OFFSET)
                self.assertEqual(stream.read(len(spl)), spl)
            for name, expected in (("kernel.bin", image_header), (pine.DTB_NAME, dtb())):
                result = subprocess.run(["mcopy", "-i", f"{output}@@{pine.PARTITION_OFFSET}",
                                         "::/" + name, "-"], capture_output=True, check=True)
                self.assertEqual(result.stdout, expected)
            script = subprocess.run(["mcopy", "-i", f"{output}@@{pine.PARTITION_OFFSET}",
                                     "::/boot.scr", "-"], capture_output=True, check=True).stdout
            self.assertEqual(script[:4], b"\x27\x05\x19\x56")
            staged = json.loads((bundle / "manifest.json").read_text())
            self.assertEqual(staged["files"][pine.DTB_NAME], pine.artifact(bundle / pine.DTB_NAME))
            # A stale or changed DTB must fail before packaging.
            (firmware / pine.DTB_NAME).write_bytes(dtb("pine64,pine64-plus"))
            with self.assertRaises(pine.PrepareError):
                pine.validate_firmware(firmware)


if __name__ == "__main__":
    unittest.main()

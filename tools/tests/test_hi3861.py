"""Hi3861 board selection and kernel-A image format checks."""
from __future__ import annotations

from pathlib import Path
import struct
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from a20_instance import applicable_actions, parse_instance
from a20_registry import load_trim_registry, plan_trim
from a20_validate import validate_instance
from hi3861 import IMAGE_BASE, build_hupg, flat_image, verify_hupg

ROOT = Path(__file__).resolve().parents[2]


class Hi3861Configuration(unittest.TestCase):
    def test_instance_uses_the_mcu_profile_and_is_physical_only(self) -> None:
        inst = parse_instance(ROOT / "instances/hi3861.toml")
        self.assertEqual(validate_instance(inst, ROOT), [])
        self.assertEqual(plan_trim(inst, load_trim_registry(ROOT))[0].state, "mcu")
        actions = applicable_actions(inst)
        self.assertIn("build", actions)
        self.assertIn("prepare", actions)
        self.assertIn("flash", actions)
        self.assertIn("package", actions)
        self.assertIn("console", actions)
        self.assertNotIn("run", actions)

    def test_hi3861_requires_riscv32_mcu_mode(self) -> None:
        from dataclasses import replace
        inst = parse_instance(ROOT / "instances/hi3861.toml")
        self.assertTrue(validate_instance(replace(inst, kernel=replace(inst.kernel, bringup=False)), ROOT))
        self.assertTrue(validate_instance(replace(inst, arch="riscv64"), ROOT))

    def test_platform_page_exists_in_the_standard_location(self) -> None:
        self.assertTrue((ROOT / "docs/platforms/hi3861.md").is_file())


class Hi3861Image(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.elf = Path(self.tmp.name) / "test.elf"
        code = b"\x13\x00\x00\x00"
        data = b"\xef\xbe\xad\xde"
        header = struct.pack(
            "<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + bytes(9),
            2, 243, 1, IMAGE_BASE, 52, 0, 1, 52, 32, 2, 40, 0, 0)
        ph_code = struct.pack("<IIIIIIII", 1, 116, IMAGE_BASE, IMAGE_BASE,
                              len(code), len(code), 5, 4)
        ph_data = struct.pack("<IIIIIIII", 1, 120, 0x000D8000, IMAGE_BASE + 8,
                              len(data), 8, 6, 4)
        self.elf.write_bytes(header + ph_code + ph_data + code + data)

    def test_flat_image_uses_flash_lmas_and_skips_bss(self) -> None:
        self.assertEqual(flat_image(self.elf), b"\x13\x00\x00\x00\x00\x00\x00\x00\xef\xbe\xad\xde")

    def test_kernel_a_header_and_signatures_round_trip(self) -> None:
        payload = flat_image(self.elf)
        image = build_hupg(payload)
        self.assertEqual(image[:8], struct.pack("<II", 0x3C78961E, 0))
        self.assertEqual(len(image), (0x3C0 + len(payload) + 15) & ~15)
        record = verify_hupg(image, payload)
        self.assertEqual(record["payload_offset"], 0x3C0)
        self.assertEqual(record["payload_bytes"], len(payload))

    def test_damaged_header_or_payload_is_rejected(self) -> None:
        payload = flat_image(self.elf)
        image = build_hupg(payload)
        for at in (0, 48, 96, 0x180, 0x280, 0x2C0, 0x3C0, len(image) - 1):
            damaged = bytearray(image)
            damaged[at] ^= 1
            with self.assertRaises(ValueError):
                verify_hupg(bytes(damaged), payload)

    def test_wrong_entry_and_bad_abi_are_rejected(self) -> None:
        original = self.elf.read_bytes()
        for offset, value in ((24, IMAGE_BASE + 4), (36, 4)):
            damaged = bytearray(original)
            struct.pack_into("<I", damaged, offset, value)
            self.elf.write_bytes(damaged)
            with self.assertRaises(ValueError):
                flat_image(self.elf)


if __name__ == "__main__":
    unittest.main()

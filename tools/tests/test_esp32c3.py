"""C3 configuration boundaries and executable ROM image validation."""
from dataclasses import replace
import hashlib
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import unittest
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "cli"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "boards"))
from a20_instance import parse_instance
from a20_registry import load_trim_registry, plan_trim, validate_trim_registry
from a20_validate import validate_instance
from esp32c3 import elf_segments, verify_image

ROOT = Path(__file__).resolve().parents[2]


class C3Configuration(unittest.TestCase):
    def test_board_selection_does_not_trim_hosted_rv32(self):
        reg = load_trim_registry(ROOT)
        for instance, expected in (("esp32-c3", "mcu"), ("qemu-riscv32-mcu", "mcu"),
                                   ("qemu-riscv32", "full"), ("qemu-riscv32-nommu", "full"),
                                   ("stm32f103", "mcu")):
            inst = parse_instance(ROOT / "instances" / f"{instance}.toml")
            self.assertEqual(plan_trim(inst, reg)[0].state, expected)
            self.assertEqual(validate_instance(inst, ROOT), [])

    def test_soft_float_execution_abi_cannot_match_double_float_image(self):
        source = r'''#include <assert.h>
#include "uapi/a20/execution.h"
int main(void) {
    a20_execution_info_t i = {
        .size=A20_EXECUTION_BYTES, .version=A20_EXECUTION_VERSION,
        .elf_machine=243, .pointer_bits=32, .byte_order=A20_EXEC_LITTLE_ENDIAN,
        .calling_abi=A20_EXEC_ABI_RV32_SOFT, .native_major=1, .features=A20_EXEC_NATIVE_IMAGE,
        .memory_model=A20_EXEC_MEMORY_FLAT, .atomic_model=A20_EXEC_ATOMIC_NATIVE};
    assert(a20_execution_valid(&i));
    a20_execution_requirement_t r = {
        .elf_machine=243, .pointer_bits=32, .byte_order=A20_EXEC_LITTLE_ENDIAN,
        .calling_abi=A20_EXEC_ABI_RV32_DOUBLE, .native_major=1};
    assert(!a20_execution_image_matches(&i, &r));
    r.calling_abi=A20_EXEC_ABI_RV32_SOFT;
    assert(a20_execution_image_matches(&i, &r));
    i.pointer_bits=64;
    assert(!a20_execution_valid(&i));
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as folder:
            src = Path(folder) / "abi.c"
            binary = Path(folder) / "abi"
            src.write_text(source)
            subprocess.run(["cc", "-Wall", "-Wextra", "-Werror", "-I" + str(ROOT / "kernel/include"),
                            str(src), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

    def test_bad_board_selectors_are_rejected(self):
        reg = load_trim_registry(ROOT)
        for board in ("riscv32/../esp32-c3", "unknown/esp32-c3", "riscv32/missing"):
            profile = replace(reg.profiles["mcu"], boards=(board,))
            changed = replace(reg, profiles={"mcu": profile})
            self.assertTrue(validate_trim_registry(changed, ROOT))

    def test_mcu_rejects_unsupported_runtime_and_smp(self):
        inst = parse_instance(ROOT / "instances/esp32-c3.toml")
        for changed in (replace(inst, abi="linux"),
                        replace(inst, machine=replace(inst.machine, smp=2, allow_unverified_smp=True)),
                        replace(inst, kernel=replace(inst.kernel, profile="full")),
                        replace(inst, kernel=replace(inst.kernel, nommu=False))):
            self.assertTrue(validate_instance(changed, ROOT))

    def test_deploy_dry_run_never_opens_serial(self):
        result = subprocess.run([str(ROOT / "tools/a20"), "deploy", "esp32-c3",
                                 "--serial", "/dev/a20-nonexistent", "--dry-run"],
                                cwd=ROOT, capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ESPTOOL_PORT=/dev/a20-nonexistent", result.stdout)
        self.assertIn("reset --port /dev/a20-nonexistent", result.stdout)
        self.assertIn("would require", result.stdout)


class C3Image(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.elf = Path(self.tmp.name) / "test.elf"
        self.image = Path(self.tmp.name) / "test.bin"
        payload = b"\x13\x00\x00\x00" * 4
        header = struct.pack("<16sHHIIIIIHHHHHH", b"\x7fELF\x01\x01\x01" + bytes(9),
                             2, 243, 1, 0x40380000, 52, 0, 1, 52, 32, 1, 0, 0, 0)
        ph = struct.pack("<IIIIIIII", 1, 84, 0x40380000, 0x40380000, len(payload), len(payload), 5, 4)
        self.elf.write_bytes(header + ph + payload)
        h = bytearray(24)
        h[0], h[1], h[23] = 0xe9, 1, 1
        struct.pack_into("<I", h, 4, 0x40380000)
        struct.pack_into("<H", h, 12, 5)
        data = h + struct.pack("<II", 0x40380000, len(payload)) + payload
        checksum = 0xef
        for b in payload: checksum ^= b
        data += bytes(((len(data) + 16) & ~15) - len(data) - 1) + bytes([checksum])
        self.image.write_bytes(data + hashlib.sha256(data).digest())

    def test_complete_image_matches_elf(self):
        self.assertEqual(verify_image(self.image, self.elf)["entry"], 0x40380000)

    def test_wrong_chip_checksum_hash_and_truncation(self):
        original = self.image.read_bytes()
        for at in (0, 4, 12, 32, len(original) - 1):
            damaged = bytearray(original)
            damaged[at] ^= 1
            self.image.write_bytes(damaged)
            with self.assertRaises(ValueError): verify_image(self.image, self.elf)
        for size in (0, 23, 25, len(original) - 1):
            self.image.write_bytes(original[:size])
            with self.assertRaises(ValueError): verify_image(self.image, self.elf)

    def test_hard_float_and_out_of_budget_elf_rejected(self):
        original = self.elf.read_bytes()
        for offset, value in ((36, 5), (64, 0x403d0000), (72, 0x400000)):
            damaged = bytearray(original)
            struct.pack_into("<I", damaged, offset, value)
            self.elf.write_bytes(damaged)
            with self.assertRaises(ValueError): elf_segments(self.elf)


if __name__ == "__main__":
    unittest.main()

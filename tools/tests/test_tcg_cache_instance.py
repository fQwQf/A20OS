"""TCG cache budgets must survive typed manifests and actual Make argv."""
from pathlib import Path
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(ROOT / "tools/cli"))
from a20_instance import InstanceError, parse_instance
from a20_derive import derive_make_vars
from a20_validate import validate_instance
from a20_test import _qemu_cmdline


class TcgCacheInstance(unittest.TestCase):
    def manifest(self, directory, value, accel='"tcg"'):
        p = Path(directory) / "cache.toml"
        p.write_text('arch="riscv64"\nabi="both"\n[machine]\nsmp=2\n' +
                     (f'accel={accel}\n' if accel else '') +
                     (f'tcg_tb_size_mb={value}\n' if value is not None else ''))
        return parse_instance(p)

    def test_exact_accelerator_argument_and_default(self):
        with tempfile.TemporaryDirectory(prefix="a20-tcg-cache-") as directory:
            for size in (1, 128, 4096):
                inst = self.manifest(directory, size)
                self.assertEqual(validate_instance(inst, ROOT), [])
                self.assertIn(f'QEMU_ACCEL=tcg,tb-size={size}', derive_make_vars(inst))
                argv = _qemu_cmdline(inst)
                self.assertEqual(argv.count('-accel'), 1)
                self.assertEqual(argv[argv.index('-accel') + 1], f'tcg,tb-size={size}')
            inst = self.manifest(directory, None)
            self.assertIn('QEMU_ACCEL=tcg', derive_make_vars(inst))
            self.assertFalse(any('tb-size' in s for s in _qemu_cmdline(inst)))

    def test_invalid_budgets_or_accelerators_are_rejected(self):
        with tempfile.TemporaryDirectory(prefix="a20-tcg-cache-") as directory:
            for size in (0, -1, 4097):
                self.assertTrue(any('tcg_tb_size_mb' in error for error in
                                    validate_instance(self.manifest(directory, size), ROOT)))
            for accel in ('"kvm"', ''):
                self.assertTrue(any('requires machine.accel=tcg' in error for error in
                                    validate_instance(self.manifest(directory, 128, accel), ROOT)))
            for literal in ('true', '"128"', '128.5'):
                with self.assertRaises(InstanceError):
                    self.manifest(directory, literal)


if __name__ == '__main__':
    unittest.main()

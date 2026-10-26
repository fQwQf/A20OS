"""A physical PC package must produce the path its deploy command will write."""
import sys
import unittest
from dataclasses import replace
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from a20_board import run_package
from a20_instance import parse_instance
from a20_derive import derive_make_vars
from a20_validate import validate_instance
from a20_make import build_instance

ROOT = Path(__file__).resolve().parents[2]


class PcPackagePaths(unittest.TestCase):
    def test_physical_images_use_their_declared_output(self):
        cases = (
            ('huawei-ct5100', 'pc-uefi-world-disk', 'PC_UEFI_OUTPUT'),
            ('huawei-ct5100-recovery', 'x86_64-uefi-disk', 'PC_UEFI_OUTPUT'),
            ('huawei-ct5100-bios', 'pc-rescue-iso', 'PC_RESCUE_ISO'),
            ('j1900-pc', 'pc-rescue-iso', 'PC_RESCUE_ISO'),
        )
        for name, target, variable in cases:
            with self.subTest(name=name):
                inst = parse_instance(ROOT / 'instances' / (name + '.toml'))
                with patch('a20_board.exec_make') as make:
                    run_package(inst, ['KERNEL_WERROR=1'], True)
                make.assert_called_once_with(
                    inst, target,
                    ['KERNEL_WERROR=1', f'{variable}={inst.target.boot_media[0]}'], True)

    def test_world_requires_explicit_distro_mount_policy(self):
        inst = parse_instance(ROOT / 'instances/huawei-ct5100.toml')
        self.assertEqual(validate_instance(inst, ROOT), [])
        self.assertIn('RAMFS_DISTRO=1', derive_make_vars(inst))
        with patch('a20_make.exec_make') as make:
            build_instance(inst, [], True)
            make.assert_called_once_with(inst, 'image-world', [], True)
        for kernel in (replace(inst.kernel, ramfs_user=False),
                       replace(inst.kernel, ramfs_distro=False),
                       replace(inst.kernel, storage_read_only=True),
                       replace(inst.kernel, ramfs_media=True),
                       replace(inst.kernel, nommu=True)):
            self.assertTrue(validate_instance(replace(inst, kernel=kernel), ROOT))


if __name__ == '__main__':
    unittest.main()

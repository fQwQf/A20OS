"""Real board serial-shell profiles must build their embedded root filesystem."""
from pathlib import Path
import sys
import unittest
from dataclasses import replace
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from a20_board import firmware_make_args, run_package
from a20_derive import derive_make_vars
from a20_instance import parse_instance
from a20_make import REPO_ROOT, build_instance
from a20_manifest import _candidates
from a20_validate import validate_instance


class BoardShellProfiles(unittest.TestCase):
    def test_physical_media_profiles_select_players_and_matching_firmware(self):
        for board in ('pine-a64', 'quartz64-zero', 'ls2k0300'):
            with self.subTest(board=board):
                inst = parse_instance(REPO_ROOT / 'instances' / f'{board}-media.toml')
                self.assertEqual(validate_instance(inst, REPO_ROOT), [])
                self.assertIn('RAMFS_MEDIA=1', derive_make_vars(inst))
                with patch('a20_board.exec_make') as execute:
                    run_package(inst, [], False)
                    extra = execute.call_args.args[2]
                    if board == 'quartz64-zero':
                        self.assertEqual(firmware_make_args(inst), ['QUARTZ64_ZERO_VIDEO=1'])
                        self.assertIn('QUARTZ64_ZERO_VIDEO=1', extra)
                        self.assertIn(('firmware-manifest', 'build/quartz64-zero/firmware-video/manifest.json'),
                                      _candidates(inst, {'KERNEL_ELF': 'kernel.elf', 'KERNEL_BIN': 'kernel.bin'}))
                    else:
                        self.assertEqual(firmware_make_args(inst), [])
                invalid = replace(inst, package=replace(inst.package, kind='board-sdcard', variant='unknown'))
                self.assertTrue(any('package.variant' in e for e in validate_instance(invalid, REPO_ROOT)))

    def test_media_profile_is_explicit_and_requires_ramfs(self):
        inst = parse_instance(REPO_ROOT / 'instances/j1900-pc-uefi-shell.toml')
        self.assertEqual(validate_instance(inst, REPO_ROOT), [])
        self.assertIn('RAMFS_MEDIA=1', derive_make_vars(inst))
        invalid = replace(inst, kernel=replace(inst.kernel, ramfs_user=False))
        self.assertTrue(any('ramfs_media requires' in e for e in validate_instance(invalid, REPO_ROOT)))

    def test_media_and_plain_shell_build_directories_are_distinct(self):
        from a20_make import query_make
        inst = parse_instance(REPO_ROOT / 'instances/j1900-pc-uefi-shell.toml')
        plain = replace(inst, kernel=replace(inst.kernel, ramfs_media=False))
        media_vars = query_make(inst, ['BUILD_DIR', 'CFLAGS'])
        plain_vars = query_make(plain, ['BUILD_DIR', 'CFLAGS'])
        self.assertNotEqual(media_vars['BUILD_DIR'], plain_vars['BUILD_DIR'])
        self.assertIn('-DCONFIG_RAMFS_MEDIA', media_vars['CFLAGS'])
        self.assertNotIn('-DCONFIG_RAMFS_MEDIA', plain_vars['CFLAGS'])

    def test_shell_kernel_and_package_configuration(self):
        for board in ('pine-a64', 'quartz64-zero', 'licheerv-nano', 'ox64', 'ls2k0300', 'lichee-pi-zero'):
            with self.subTest(board=board):
                inst = parse_instance(REPO_ROOT / 'instances' / f'{board}-shell.toml')
                variables = derive_make_vars(inst)
                self.assertIn('BRINGUP=0', variables)
                self.assertIn('RAMFS_USER=1', variables)
                self.assertIn('DRIVER_DEPLOYMENT=embedded', variables)
                with patch('a20_make.exec_make') as execute:
                    build_instance(inst, [], False)
                    self.assertEqual(execute.call_args.args[1], 'kernel-only')
                with patch('a20_board.exec_make') as execute:
                    run_package(inst, [], False)
                    extra = execute.call_args.args[2]
                    if board != 'ls2k0300':
                        self.assertIn(f'HANDOFF_INSTANCE={inst.source}', extra)
                        self.assertTrue(any(x.endswith(inst.target.boot_media[0]) for x in extra))

    def test_ramfs_ledger_does_not_require_disk_images(self):
        inst = parse_instance(REPO_ROOT / 'instances/ox64-shell.toml')
        paths = _candidates(inst, {'KERNEL_ELF': 'kernel.elf', 'KERNEL_BIN': 'kernel.bin'})
        self.assertFalse(any(role.startswith('rootfs-') for role, _ in paths))

    def test_shell_and_recovery_images_are_distinct(self):
        for board in ('pine-a64', 'quartz64-zero', 'licheerv-nano', 'ox64', 'lichee-pi-zero'):
            recovery = parse_instance(REPO_ROOT / 'instances' / f'{board}.toml')
            shell = parse_instance(REPO_ROOT / 'instances' / f'{board}-shell.toml')
            self.assertNotEqual(shell.target.boot_media, recovery.target.boot_media)

class MinimalUserStamp(unittest.TestCase):
    def test_only_selected_programs_are_built_and_missing_one_rebuilds(self):
        import argparse
        import importlib.util
        import tempfile
        spec = importlib.util.spec_from_file_location('minimal_stamps', REPO_ROOT / 'tools/stamps.py')
        stamps = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(stamps)
        with tempfile.TemporaryDirectory() as directory:
            repo = Path(directory)
            stamps.REPO = repo
            output = repo / 'user/build/riscv64-ramfs'
            args = argparse.Namespace(build_id='ramfs-id', stamp=str(output / '.ramfs-build-id'),
                                      user_build_dir=str(output), arch='riscv64', nommu='0',
                                      user_opt='-O3', profile='full', user_variant='riscv64-ramfs',
                                      roots='', skip='', envelope_acquire_race_test='0',
                                      syscall_smoke_task_construction_test='0', targets='init mksh echo')
            commands = []
            def make(command):
                commands.append(command)
                if command[-1] != 'clean':
                    output.mkdir(parents=True, exist_ok=True)
                    for name in ('init', 'mksh', 'echo'):
                        path = output / name
                        path.write_text('elf')
                        path.chmod(0o755)
                return 0
            stamps.submake = make
            self.assertEqual(stamps.cmd_user(args), 0)
            self.assertEqual(commands[-1][-3:], [str(output / name) for name in ('init', 'mksh', 'echo')])
            commands.clear()
            self.assertEqual(stamps.cmd_user(args), 0)
            self.assertEqual(commands, [])
            (output / 'echo').unlink()
            self.assertEqual(stamps.cmd_user(args), 0)
            self.assertEqual(len(commands), 1)
            self.assertNotEqual(commands[0][-1], 'clean')


if __name__ == '__main__':
    unittest.main()

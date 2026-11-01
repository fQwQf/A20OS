"""Structural architecture ownership and shared boot/firmware boundary."""
import sys
import tempfile
import subprocess
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from a20_boards import board_directory
from check_arch_layout import violations
from a20_instance import ACTIVE_ARCHES, RETIRED_ARCHES

class ArchLayoutTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)

    def write(self, name, data=''):
        p = self.root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_text(data)
        return p

    def board(self, arch='arm32', name='sample'):
        self.write(f'kernel/arch/{arch}/boot/context.c')
        self.write(f'kernel/arch/{arch}/boards/{name}/board.c')

    def test_repository_layout(self):
        self.assertEqual(violations(ROOT), [])

    def test_every_active_board_builds_the_common_boot_contract(self):
        """Inspect actual Make source graphs, including small MCU profiles."""
        checked = []
        for directory in sorted((ROOT/'kernel/arch').glob('*/boards/*')):
            arch, board = directory.parent.parent.name, directory.name
            if arch not in ACTIVE_ARCHES or board == 'm0sense-bl702' or arch == 'riscv32':
                continue  # Retired targets, the standalone BL702 probe, and the MCU boot graph are not hosted OS ports.
            with self.subTest(arch=arch, board=board):
                result = subprocess.run(['make', '-s', f'ARCH={arch}', f'BOARD={board}',
                    'BRINGUP=1', 'DRIVER_DEPLOYMENT=embedded',
                    *(['CROSS_PREFIX=arm-none-eabi-'] if arch == 'armv7m' else []),
                    *(['ABI=native'] if arch == 'riscv32' else []),
                    '--eval=print-boot-graph:;@echo $(KERNEL_SRC) $(ASM_SRC)',
                    'print-boot-graph'], cwd=ROOT, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                sources = result.stdout.split()
                self.assertIn('kernel/boot/context.c', sources)
                self.assertIn(f'kernel/arch/{arch}/boot/context.c', sources)
                self.assertIn('kernel/firmware/node.c', sources)
                entry = f'kernel/arch/{arch}/boards/{board}/mcu_main.c' if arch == 'armv7m' else 'kernel/main.c'
                self.assertIn(entry, sources)
                if arch != 'armv7m':
                    self.assertIn('kernel/firmware/device.c', sources)
                    self.assertIn('kernel/drivers/core/driver_core.c', sources)
                self.assertFalse(any('/boards/' in s and f'/boards/{board}/' not in s for s in sources))
                self.assertFalse(any('/boot/uefi/' in s or s.endswith('/virtio_probe.c') for s in sources))
                checked.append(board)
        self.assertTrue(checked, 'the build graph audit must cover real board targets')

    def test_retired_architectures_cannot_be_reported_as_active_support(self):
        for arch in RETIRED_ARCHES:
            result = subprocess.run(['make', '-s', f'ARCH={arch}', 'kernel-only'],
                                    cwd=ROOT, text=True, capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn('retired from active builds', result.stderr)

    def test_make_selects_one_board_and_excludes_standalone_efi(self):
        for arch, board in [('aarch64', 'pine-a64'), ('riscv64', 'licheerv-nano'),
                            ('x86_64', 'x86_64-pc')]:
            with self.subTest(arch=arch, board=board):
                result = subprocess.run(['make', '-s', f'ARCH={arch}', f'BOARD={board}',
                    'BRINGUP=1', '--eval=print-layout-sources:;@echo $(KERNEL_SRC) $(ASM_SRC)',
                    'print-layout-sources'], cwd=ROOT, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                sources = result.stdout.split()
                self.assertTrue(any(f'/boards/{board}/' in x for x in sources))
                self.assertFalse(any('/boot/uefi/' in x for x in sources))
                self.assertFalse(any('/boards/' in x and f'/boards/{board}/' not in x for x in sources))

    def test_wrong_arch_does_not_resolve_board(self):
        self.board()
        self.assertIsNotNone(board_directory(self.root, 'sample', 'arm32'))
        self.assertIsNone(board_directory(self.root, 'sample', 'aarch64'))
        self.assertIsNone(board_directory(self.root, '../sample'))

    def test_no_global_fallback(self):
        self.write('kernel/platform/sample/board.c')
        self.assertIsNone(board_directory(self.root, 'sample'))
        self.assertTrue(any('kernel/platform' in e for e in violations(self.root)))

    def test_private_transport_scan_cannot_return_inside_arch(self):
        self.board()
        self.write('kernel/arch/arm32/platform/probe.c',
                   'int arch_virtio_blk_probe(int index, void *out);')
        self.assertTrue(any('obsolete private VirtIO discovery' in e for e in violations(self.root)))

    def test_duplicate_board_rejected(self):
        self.board(); self.board('aarch64')
        self.assertIsNone(board_directory(self.root, 'sample'))
        self.assertTrue(any('duplicate' in e for e in violations(self.root)))

    def test_shared_firmware_cannot_use_isa_headers_or_instructions(self):
        self.write('kernel/firmware/bad.c', '#include "platform.h"\n__asm__ volatile("dsb sy");')
        errors = violations(self.root)
        self.assertTrue(any('implementation include' in e for e in errors))
        self.assertTrue(any('CPU instructions' in e for e in errors))

    def test_cross_isa_board_include(self):
        self.board()
        self.write('kernel/arch/arm32/boards/sample/board.c', '#include "arch/aarch64/include/cpu.h"')
        self.assertTrue(any('cross-architecture' in e for e in violations(self.root)))

    def test_efi_loader_cannot_return_to_shared_boot(self):
        self.write('kernel/boot/uefi/loader.c')
        self.assertTrue(any('EFI loaders' in e for e in violations(self.root)))

    def test_stm32_drivers_and_private_headers_are_board_owned(self):
        self.write('kernel/drivers/stm32f1/uart.c')
        self.write('kernel/include/drivers/stm32f1/uart.h')
        self.assertEqual(sum('STM32 drivers' in e for e in violations(self.root)), 2)

    def test_common_mcu_has_no_board_entry_or_private_includes(self):
        self.write('kernel/mcu/main.c', '#include "board.h"\n#include "drivers/stm32f1/extsram.h"\n')
        errors = violations(self.root)
        self.assertTrue(any('MCU hardware entry' in e for e in errors))
        self.assertEqual(sum('implementation include' in e for e in errors), 2)

    def test_mcu_source_graph_uses_board_entry_and_private_drivers(self):
        result = subprocess.run(['make', '-s', 'ARCH=armv7m', 'BOARD=stm32f103',
            'CROSS_PREFIX=arm-none-eabi-', 'BRINGUP=1',
            '--eval=print-layout-sources:;@echo $(KERNEL_SRC)', 'print-layout-sources'],
            cwd=ROOT, text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stderr)
        sources = result.stdout.split()
        self.assertIn('kernel/arch/armv7m/boards/stm32f103/mcu_main.c', sources)
        self.assertIn('kernel/arch/armv7m/boards/stm32f103/drivers/extsram.c', sources)
        self.assertNotIn('kernel/mcu/main.c', sources)

    def test_vfs_rust_objects_follow_their_frontend(self):
        for arch, board, expected in [('armv7m', 'stm32f103', False),
                                      ('x86_64', 'x86_64-pc', True)]:
            with self.subTest(arch=arch):
                result = subprocess.run(['make', '-s', f'ARCH={arch}', f'BOARD={board}',
                    'CROSS_PREFIX=arm-none-eabi-' if arch=='armv7m' else 'CROSS_PREFIX=x86_64-linux-gnu-',
                    'BRINGUP=1', '--eval=print-vfs-objects:;@echo $(KERNEL_OBJ)', 'print-vfs-objects'],
                    cwd=ROOT, text=True, capture_output=True)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertEqual(any(x.endswith('/rust/vfs_lifecycle.o')
                                     for x in result.stdout.split()), expected)

if __name__ == '__main__':
    unittest.main()

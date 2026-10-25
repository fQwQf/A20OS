"""Console memory profiles: manifest wiring, validation and real DMA layouts."""
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'tools'))
from a20_instance import InstanceError, parse_instance
from a20_derive import derive_make_vars
from a20_validate import validate_instance
from virtio_console_config import MACROS, PRESETS, header, resolve


class VportConfigTest(unittest.TestCase):
    def test_presets_and_overrides(self):
        self.assertEqual(resolve(), PRESETS['throughput'])
        self.assertEqual(resolve('compact'), PRESETS['compact'])
        self.assertEqual(resolve('compact', tx_slots=8, ring_size=None),
                         dict(rx_slots=2, tx_slots=8, buffer_size=512, ring_size=512))

    def test_invalid_values(self):
        for profile in ('unknown', ''):
            with self.subTest(profile=profile), self.assertRaises(ValueError):
                resolve(profile)
        for name, bad in [('rx_slots', 0), ('rx_slots', 3), ('tx_slots', 32),
                          ('tx_slots', True), ('buffer_size', 32),
                          ('buffer_size', 8192), ('ring_size', 127),
                          ('ring_size', 131072), ('ring_size', '512')]:
            with self.subTest(name=name, value=bad), self.assertRaises(ValueError):
                resolve(**{name: bad})

    def test_manifest_validation_and_make_variables(self):
        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / 'console.toml'
            path.write_text('arch="x86_64"\n[kernel]\n'
                            'virtio_console_profile="compact"\n'
                            'virtio_console_rx_slots=1\nvirtio_console_tx_slots=4\n'
                            'virtio_console_buffer_size=256\nvirtio_console_ring_size=1024\n')
            inst = parse_instance(path)
            self.assertEqual(validate_instance(inst, ROOT), [])
            values = derive_make_vars(inst)
            for flag in ['PROFILE=compact', 'RX_SLOTS=1', 'TX_SLOTS=4',
                         'BUFFER_SIZE=256', 'RING_SIZE=1024']:
                self.assertIn('VIRTIO_CONSOLE_' + flag, values)
            for setting in ['virtio_console_tx_slots=3', 'virtio_console_profile="bad"']:
                path.write_text('arch="x86_64"\n[kernel]\n' + setting + '\n')
                self.assertTrue(any('kernel.virtio_console_' in e
                                    for e in validate_instance(parse_instance(path), ROOT)))
            path.write_text('arch="x86_64"\n[kernel]\nvirtio_console_tx_slots=true\n')
            with self.assertRaises(InstanceError):
                parse_instance(path)

    def test_header_changes_only_when_resolved_configuration_changes(self):
        with tempfile.TemporaryDirectory() as tmp:
            output = Path(tmp) / 'config.h'
            argv = [sys.executable, str(ROOT / 'tools/virtio_console_config.py'),
                    '--output', str(output)]
            subprocess.run(argv, check=True)
            self.assertEqual(output.read_text(), header(resolve()))
            os.utime(output, ns=(1000000000, 1000000000))
            subprocess.run(argv, check=True)
            self.assertEqual(output.stat().st_mtime_ns, 1000000000)
            subprocess.run([*argv, '--profile', 'compact'], check=True)
            self.assertEqual(output.read_text(), header(resolve('compact')))
            before = output.read_bytes()
            result = subprocess.run([*argv, '--tx-slots', '3'], capture_output=True)
            self.assertNotEqual(result.returncode, 0)
            self.assertEqual(output.read_bytes(), before)
            subprocess.run(argv, check=True)
            self.assertEqual(output.read_text(), header(resolve()))

    def test_production_queue_layout_dma_budgets(self):
        driver = (ROOT / 'kernel/drivers/char/virtio_console.c').read_text()
        constants = driver[driver.index('#define VPORT_Q_RX'):driver.index('typedef struct {')]
        structs = driver[driver.index('typedef struct {'):driver.index('typedef struct {\n    virtio_transport_t')]
        source = ('#include <stdint.h>\n#include <stdio.h>\n'
                  '#include "drivers/char/virtio_console_config.h"\n'
                  '#define ALIGNED(n) __attribute__((aligned(n)))\n' + constants + structs +
                  '\nint main(void) { printf("%zu %zu %zu\\n", sizeof(vport_rx_mem_t), '
                  'sizeof(vport_tx_mem_t), sizeof(vport_ctrl_mem_t)); }\n')
        def allocated(size):
            return 4096 * (1 << ((size + 4095) // 4096 - 1).bit_length())
        with tempfile.TemporaryDirectory() as tmp:
            cfile = Path(tmp) / 'layout.c'
            exe = Path(tmp) / 'layout'
            cfile.write_text(source)
            for profile, expected in [('throughput', (192 * 1024, 200 * 1024)),
                                      ('compact', (8 * 1024, 16 * 1024))]:
                values = resolve(profile)
                definitions = [f'-D{MACROS[k]}={v}' for k, v in values.items()]
                subprocess.run(['cc', f'-I{ROOT}/kernel/include', *definitions,
                                str(cfile), '-o', str(exe)], check=True)
                rx, tx, ctrl = map(int, subprocess.check_output([str(exe)]).split())
                raw = allocated(rx) + allocated(tx)
                self.assertEqual((raw, raw + 2 * allocated(ctrl)), expected)
            for name, value in [('rx_slots', 3), ('tx_slots', 32),
                                ('buffer_size', 32), ('ring_size', 127)]:
                result = subprocess.run(['cc', f'-I{ROOT}/kernel/include',
                                         f'-D{MACROS[name]}={value}', '-fsyntax-only',
                                         str(cfile)], capture_output=True)
                self.assertNotEqual(result.returncode, 0)


if __name__ == '__main__':
    unittest.main()

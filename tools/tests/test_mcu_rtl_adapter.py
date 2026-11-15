"""Check real FSMC RTL byte lanes through the compiled cosimulation adapter."""
from pathlib import Path
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
BACKEND = ROOT / '.kernel-build/cluster/shared-rtl-verilator/Va20_shared_top'


@unittest.skipUnless(BACKEND.is_file(), 'requires make build-mcu-shared-rtl')
class RtlAdapterTests(unittest.TestCase):
    def test_unaligned_ram_beats_preserve_neighbor_bytes_across_cpu_ports(self):
        commands, expected = [], []
        data = bytearray(range(24))
        for offset in range(0, 24, 8):
            commands.append(f'x 0 0 1 {offset} 8 {int.from_bytes(data[offset:offset+8], "little")}')
            expected.append(None)  # write response is not a readback oracle
        for size in (1, 2, 4, 8):
            value = int.from_bytes(bytes(0xf0 + i for i in range(size)), 'little')
            commands.append(f'x 1 0 1 5 {size} {value}')
            expected.append(None)
            data[5:5+size] = value.to_bytes(size, 'little')
            for cpu in (0, 1):
                commands.append(f'x {cpu} 0 0 5 {size} 0')
                expected.append(value)
                for offset in range(0, 24, 8):
                    commands.append(f'x {cpu} 0 0 {offset} 8 0')
                    expected.append(int.from_bytes(data[offset:offset+8], 'little'))
        result = subprocess.run([str(BACKEND)], input='\n'.join(commands + ['q']) + '\n',
                                text=True, capture_output=True, timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        replies = [int(line) for line in result.stdout.splitlines()]
        self.assertEqual(len(replies), len(expected))
        for actual, wanted in zip(replies, expected):
            if wanted is not None:
                self.assertEqual(actual, wanted)

    def test_control_register_unaligned_and_address_wrap_are_rejected(self):
        for request in ('x 0 1 0 1 2 0', 'x 0 0 0 67108863 8 0'):
            with self.subTest(request=request):
                result = subprocess.run([str(BACKEND)], input=request + '\n',
                                        text=True, capture_output=True, timeout=10)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn('invalid cosimulation request', result.stderr)


if __name__ == '__main__':
    unittest.main()

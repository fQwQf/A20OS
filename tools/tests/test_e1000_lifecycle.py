"""Production E1000 stop/probe/I/O with PCI and PCIe DMA ownership models."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    found = re.search(r'^(?:static )?(?:inline )?(?:int|unsigned|void|size_t|uint32_t|const uint8_t)\s+\*?' +
                      name + r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
    if not found:
        raise AssertionError(f'missing production function {name}')
    return found.group()


class E1000LifecycleTests(unittest.TestCase):
    def test_stop_ack_retains_dma_and_tx_requires_device_writeback(self):
        source = (ROOT / 'kernel/drivers/net/e1000.c').read_text()
        lines = source.splitlines(keepends=True)
        defines, i = [], 0
        while i < len(lines):
            if lines[i].startswith('#define E1000_'):
                while True:
                    line = lines[i]; defines.append(line); i += 1
                    if not line.rstrip().endswith('\\'):
                        break
            else:
                i += 1
        types = source[source.index('typedef struct {'):source.index('static inline uint32_t e1000_read')]
        names = ('e1000_read', 'e1000_write', 'e1000_hardware_stop',
                 'e1000_ring_bytes', 'e1000_free_rings', 'e1000_alloc_rings',
                 'e1000_reclaim_tx_locked', 'e1000_irq_handler', 'e1000_poll',
                 'e1000_class_rx_irq_driven', 'e1000_send', 'e1000_recv',
                 'e1000_mac', 'e1000_probe', 'e1000_stop', 'e1000_remove', 'e1000_link_up')
        generated = (ROOT / 'tools/tests/e1000_lifecycle_host.c').read_text()
        generated = generated.replace('/* PRODUCTION_DEFINITIONS */', ''.join(defines) + types)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(function(source, n) for n in names))
        with tempfile.TemporaryDirectory(prefix='a20-e1000-lifecycle-') as temp:
            c, exe = Path(temp) / 'check.c', Path(temp) / 'check'
            c.write_text(generated)
            built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-function', '-fsanitize=address,undefined',
                                    '-fno-omit-frame-pointer', str(c), '-o', str(exe)],
                                   capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

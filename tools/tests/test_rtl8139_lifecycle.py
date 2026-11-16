"""Production RTL8139 probe/I/O/stop against reset, IRQ and DMA ownership models."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r'^(?:static )?(?:inline )?(?:int|void|uint8_t|uint16_t|uint32_t|const uint8_t)\s+\*?' +
                      name + r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
    if not match:
        raise AssertionError(f'missing production function {name}')
    return match.group()


class Rtl8139LifecycleTests(unittest.TestCase):
    def test_failed_reset_keeps_instance_irq_and_dma_until_retry(self):
        source = (ROOT / 'kernel/drivers/net/rtl8139.c').read_text()
        constants = source[source.index('#define RTL8139_VENDOR_REALTEK'):source.index('static int rtl8139_poll_mode')]
        types = source[source.index('typedef struct {'):source.index('static inline uint8_t rtl8139_readb')]
        align = source[source.index('#define RTL8139_ALIGN_UP'):source.index('static int rtl8139_reset')]
        names = ('rtl8139_readb', 'rtl8139_readw', 'rtl8139_readl', 'rtl8139_writeb',
                 'rtl8139_writew', 'rtl8139_writel', 'rtl8139_reset',
                 'rtl8139_free_buffers', 'rtl8139_alloc_buffers',
                 'rtl8139_reclaim_tx_locked', 'rtl8139_ring_read',
                 'rtl8139_ring_sync_read', 'rtl8139_release_ring_locked',
                 'rtl8139_irq_handler', 'rtl8139_poll', 'rtl8139_send',
                 'rtl8139_recv', 'rtl8139_mac', 'rtl8139_rx_irq_driven',
                 'rtl8139_stats_read', 'rtl8139_probe', 'rtl8139_stop', 'rtl8139_remove')
        generated = (ROOT / 'tools/tests/rtl8139_lifecycle_host.c').read_text()
        generated = generated.replace('/* PRODUCTION_DEFINITIONS */', constants + types + align)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(function(source, n) for n in names))
        with tempfile.TemporaryDirectory(prefix='a20-rtl8139-lifecycle-') as temp:
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

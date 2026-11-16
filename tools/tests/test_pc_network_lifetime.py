"""Production PCI network teardown and userspace transport ownership."""
import re
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_i2c_transport_lifetime import run, ROOT


class PcNetworkLifetime(unittest.TestCase):
    def test_radio_rebind_cannot_overwrite_live_or_quarantined_owner(self):
        source = (ROOT / 'kernel/drivers/net/rtw89_broker.c').read_text()
        definitions = source[source.index('#define WIFI_DMA_SLOTS'):
                             source.index('static int packet_push')]
        names = ('packet_push', 'packet_pop', 'wifi_send', 'wifi_recv',
                 'wifi_bus_master', 'wifi_transport_available', 'wifi_probe_locked',
                 'wifi_probe', 'wifi_stop', 'wifi_remove', 'wifi_dma_free',
                 'wifi_retire_closed_owner', 'wifi_ioctl', 'wifi_mmap', 'wifi_read',
                 'wifi_write', 'wifi_close')
        def extract(name):
            found = re.search(r'^(?:static )?[^\n]*\b' + name +
                              r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
            self.assertIsNotNone(found, f'missing production function {name}')
            return found.group()
        production = '\n'.join(extract(name) for name in names)
        production += source[source.index('static vfile_ops_t wifi_file_ops'):
                             source.index('int rtw89_file_open')]
        production += extract('rtw89_file_open')
        generated = (ROOT / 'tools/tests/rtw89_broker_lifecycle_host.c').read_text()
        generated = generated.replace('/* PRODUCTION_DEFINITIONS */', definitions)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */', production)
        with tempfile.TemporaryDirectory(prefix='a20-rtw89-lifetime-') as folder:
            code = Path(folder) / 'broker.c'
            code.write_text(generated)
            binary = code.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-fno-pie', '-no-pie', '-I', str(ROOT), str(code),
                            '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)

    def test_nic_retains_dma_after_stop_timeout_and_retry_frees_once(self):
        source = (ROOT / 'kernel/drivers/net/r8169.c').read_text()
        definitions = source[source.index('#define R8169_VENDOR_REALTEK'):
                             source.index('static int r8169_stop(void *context);')]
        names = ('r8169_read8', 'r8169_write8', 'r8169_read16', 'r8169_write16',
                 'r8169_read32', 'r8169_write32', 'r8169_desc_owned',
                 'r8169_desc_commit', 'r8169_free_rings', 'r8169_alloc_rings',
                 'r8169_rx_fill', 'r8169_reclaim_tx_locked', 'r8169_irq_handler',
                 'r8169_poll', 'r8169_send', 'r8169_rx_accept', 'r8169_recv',
                 'r8169_mac', 'r8169_link_up', 'r8169_rx_irq_driven', 'r8169_reset',
                 'r8169_random_mac', 'r8169_set_mac', 'r8169_parse_cmdline',
                 'r8169_hw_start', 'r8169_probe', 'r8169_stop', 'r8169_remove',
                 'r8169_phys_to_va', 'r8169_sim_get32', 'r8169_sim_read8',
                 'r8169_sim_deliver', 'r8169_sim_write8', 'a20_boot_selftest_r8169')
        functions = []
        for name in names:
            found = re.search(r'^(?:static )?(?:inline )?(?:int|unsigned|void|uint8_t|uint16_t|uint32_t|const uint8_t)\s+\*?' +
                              name + r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
            self.assertIsNotNone(found, f'missing production function {name}')
            functions.append(found.group())
        generated = (ROOT / 'tools/tests/r8169_lifecycle_host.c').read_text()
        generated = generated.replace('/* PRODUCTION_DEFINITIONS */', definitions)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(functions))
        run(generated)


if __name__ == '__main__':
    unittest.main()

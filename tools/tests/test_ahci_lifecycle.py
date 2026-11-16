"""Production AHCI probe, command submission and fallible stop with modeled hardware."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]


class AhciLifecycleTests(unittest.TestCase):
    def test_failed_probe_dma_stop_and_legacy_references(self):
        source = (ROOT / 'kernel/drivers/block/ahci.c').read_text()
        model = (ROOT / 'tools/tests/ahci_lifecycle_host.c').read_text()
        defines = '\n'.join(re.findall(r'^#define (?:AHCI_|ATA_CMD_).*$', source, re.M))
        types = source[source.index('typedef struct __attribute__'):source.index('/*\n * a20.ahci.poll')]
        implementations = re.sub(r'^static int ahci_stop\(void \*context\);\n', '', source, flags=re.M)
        names = ('ahci_reg', 'ahci_read', 'ahci_write', 'ahci_wait_clear',
                 'ahci_wait_ready', 'ahci_stop_port', 'ahci_start_port',
                 'ahci_issue_slot0', 'ahci_submit', 'ahci_submit_nodata',
                 'ahci_rw', 'ahci_block_read', 'ahci_block_write',
                 'ahci_block_flush', 'ahci_block_release', 'ahci_get_dev',
                 'ahci_identify', 'ahci_probe_common', 'ahci_stop', 'ahci_remove')
        generated = model.replace('/* PRODUCTION_DEFINITIONS */', defines + '\n' + types)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(function(implementations, n) for n in names))
        with tempfile.TemporaryDirectory(prefix='a20-ahci-lifecycle-') as temp:
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

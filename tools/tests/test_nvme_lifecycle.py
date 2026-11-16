"""Production NVMe probe/stop/I/O against failed-disable and DMA owners."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function

ROOT = Path(__file__).resolve().parents[2]


class NvmeLifecycleTests(unittest.TestCase):
    def test_failed_probe_busy_io_and_disable_ack(self):
        source = (ROOT / 'kernel/drivers/block/nvme.c').read_text()
        model = (ROOT / 'tools/tests/nvme_lifecycle_host.c').read_text()
        definitions = source[source.index('#define NVME_REG_CAP'):source.index('static inline volatile void *nvme_reg')]
        names = ('nvme_reg', 'nvme_capabilities_supported', 'nvme_wait_ready',
                 'nvme_queue_free', 'nvme_io_cmd', 'nvme_rw', 'nvme_flush',
                 'nvme_stop', 'nvme_release', 'nvme_probe', 'nvme_remove')
        generated = model.replace('/* PRODUCTION_DEFINITIONS */', definitions)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */',
                                      '\n'.join(function(source, n) for n in names))
        with tempfile.TemporaryDirectory(prefix='a20-nvme-lifecycle-') as temp:
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

"""Production VMSVGA command-page ownership with modeled host and PCI completion."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VmsvgaLifecycle(unittest.TestCase):
    def test_pending_command_page_is_retained_until_device_retirement(self):
        production = re.sub(r'^#include[^\n]*\n', '',
                            (ROOT / 'kernel/drivers/gpu/vmsvga.c').read_text(), flags=re.M)
        code = (ROOT / 'tools/tests/vmsvga_lifecycle_host.c').read_text().replace('/* PRODUCTION */', production)
        hardware = (ROOT / 'kernel/drivers/core/driver_hwapi.c').read_text()
        ports = '\n'.join(re.search(r'^(?:uint32_t|void) ' + name + r'\([^;{}]*\)\s*\{.*?^\}', hardware, re.M | re.S).group()
                          for name in ('ioport_read32', 'ioport_write32'))
        code = code.replace('/* PORT_PRIMITIVES */', ports)
        with tempfile.TemporaryDirectory(prefix='a20-vmsvga-lifetime-') as folder:
            path = Path(folder) / 'vmsvga.c'
            path.write_text(code)
            binary = path.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

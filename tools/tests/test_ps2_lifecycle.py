"""Execute the production PS/2 controller with modeled ports and IRQ/core boundaries."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Ps2Lifecycle(unittest.TestCase):
    def test_checked_controller_stop_and_partial_probe(self):
        production = re.sub(r'^#include[^\n]*\n', '',
                            (ROOT / 'kernel/drivers/input/ps2.c').read_text(), flags=re.M)
        code = (ROOT / 'tools/tests/ps2_lifecycle_host.c').read_text().replace('/* PRODUCTION */', production)
        with tempfile.TemporaryDirectory(prefix='a20-ps2-lifetime-') as folder:
            path = Path(folder) / 'ps2.c'
            path.write_text(code)
            binary = path.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

"""Production LS2K GMAC ring and managed teardown; hardware/core boundaries modeled."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class Ls2kGmacLifecycle(unittest.TestCase):
    def test_chained_packets_partial_probe_and_retained_stop(self):
        source = (ROOT / 'kernel/drivers/net/ls2k_gmac.c').read_text()
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        code = (ROOT / 'tools/tests/ls2k_gmac_lifecycle_host.c').read_text()
        code = code.replace('/* PRODUCTION */', source)
        with tempfile.TemporaryDirectory(prefix='a20-ls2k-gmac-') as folder:
            path = Path(folder) / 'gmac.c'
            path.write_text(code)
            binary = path.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-omit-frame-pointer',
                            '-fno-pie', '-no-pie', str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

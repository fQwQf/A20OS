"""Production polling TPM TIS FIFO and managed stop, with register boundaries modeled."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class TpmLifecycle(unittest.TestCase):
    def test_fifo_framing_timeouts_and_retained_stop(self):
        source = (ROOT / 'kernel/drvmod/examples/tpm.c').read_text()
        production = re.sub(r'^#include[^\n]*\n', '', source, flags=re.M)
        generated = (ROOT / 'tools/tests/tpm_lifecycle_host.c').read_text()
        generated = generated.replace('/* PRODUCTION */', production)
        with tempfile.TemporaryDirectory(prefix='a20-tpm-lifetime-') as folder:
            code = Path(folder) / 'tpm.c'
            code.write_text(generated)
            binary = code.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                            '-fno-pie', '-no-pie', str(code), '-o', str(binary)],
                           check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

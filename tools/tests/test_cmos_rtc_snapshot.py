"""Production CMOS probe/calendar decoding with QEMU-style index readback."""
from datetime import datetime, timezone
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CmosRtcSnapshot(unittest.TestCase):
    def test_stable_snapshot_format_fallback_and_port_ownership(self):
        code = (ROOT / 'tools/tests/cmos_rtc_snapshot_host.c').read_text()
        for marker, name in [('REGISTERS', 'kernel/include/drivers/char/cmos_rtc.h'),
                             ('PRODUCTION', 'kernel/drvmod/examples/cmos_rtc.c')]:
            source = re.sub(r'^#include[^\n]*\n', '', (ROOT / name).read_text(), flags=re.M)
            code = code.replace('/* ' + marker + ' */', source)
        for name, fields in [('BASE', (2026, 11, 17, 12, 34, 56)),
                             ('ROLLOVER', (2027, 1, 1, 0, 0, 0)),
                             ('LEAP', (2000, 2, 29, 0, 0, 0))]:
            epoch = int(datetime(*fields, tzinfo=timezone.utc).timestamp())
            code = code.replace('/* EPOCH_' + name + ' */', '#define EPOCH_' + name + ' ' + str(epoch) + 'ULL')
        with tempfile.TemporaryDirectory(prefix='a20-cmos-snapshot-') as folder:
            source = Path(folder) / 'rtc.c'
            source.write_text(code)
            binary = source.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

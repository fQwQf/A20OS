"""Production PC speaker tone/cancellation and fallible port-owner teardown."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class PcSpeakerLifecycle(unittest.TestCase):
    def test_tone_cancellation_checked_stop_and_port_claims(self):
        code = (ROOT / 'tools/tests/pc_speaker_lifecycle_host.c').read_text()
        for marker, name in [('UAPI', 'kernel/include/uapi/a20/audio.h'),
                             ('AUDIO', 'kernel/include/drivers/audio/audio_core.h'),
                             ('PRODUCTION', 'kernel/drvmod/examples/pc_spkr.c')]:
            source = re.sub(r'^#include[^\n]*\n', '', (ROOT / name).read_text(), flags=re.M)
            code = code.replace('/* ' + marker + ' */', source)
        with tempfile.TemporaryDirectory(prefix='a20-pc-speaker-') as folder:
            source = Path(folder) / 'speaker.c'
            source.write_text(code)
            binary = source.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                            str(source), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

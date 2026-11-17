"""Production HDA partial probe, PCM cancellation and fallible controller stop."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    found = re.search(r'^(?:static )?(?:volatile )?(?:int|void)\s+\*?' + name +
                      r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
    if not found:
        raise AssertionError(f'missing production function {name}')
    return found.group()


class HdaLifecycleTests(unittest.TestCase):
    def test_failed_stop_retains_binding_and_cancels_active_writer(self):
        source = (ROOT / 'kernel/drvmod/examples/hda.c').read_text()
        defines = '\n'.join(re.findall(r'^#define HDA_.*$', source, re.M))
        names = ('hda_stream_reg', 'hda_wait32', 'hda_reset', 'hda_stream_reset',
                 'hda_stream_stop', 'hda_stream_prepare', 'hda_stream_run',
                 'hda_stream_refresh', 'hda_progress', 'hda_ring_copy', 'hda_queue_pcm',
                 'hda_drain', 'hda_discard_queued', 'hda_adopt_generation',
                 'hda_write', 'hda_stop_device', 'hda_drain_device',
                 'hda_backend_ioctl', 'hda_poll', 'hda_quiesce', 'hda_stop',
                 'hda_release', 'hda_probe', 'hda_remove')
        generated = (ROOT / 'tools/tests/hda_lifecycle_host.c').read_text()
        generated = generated.replace('/* PRODUCTION_DEFINITIONS */', defines)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */', '\n'.join(function(source, n) for n in names))
        with tempfile.TemporaryDirectory(prefix='a20-hda-lifecycle-') as temp:
            work = Path(temp); (work / 'core').mkdir()
            for name in ['types.h', 'sync.h']:
                (work / 'core' / name).write_text('')
            c, exe = work / 'check.c', work / 'check'
            c.write_text(generated)
            built = subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-function', '-fsanitize=address,undefined',
                                    '-fno-omit-frame-pointer', '-I' + str(work),
                                    '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel'),
                                    str(c), '-o', str(exe)], capture_output=True, text=True)
            self.assertEqual(built.returncode, 0, built.stdout + built.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

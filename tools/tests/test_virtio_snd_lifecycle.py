"""Execute production VirtIO sound queues and managed stop with a DMA model."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VirtioSndLifecycle(unittest.TestCase):
    def test_failed_stop_retains_binding_irq_and_control_pcm_dma(self):
        source = (ROOT / 'kernel/drivers/audio/virtio_snd.c').read_text()
        code = source[source.index('#define VIRTIO_SND_MAX_STREAMS'):
                      source.index('static const audio_dev_ops_t virtio_snd_ops')]
        transport = (ROOT / 'kernel/drivers/bus/virtio_transport.c').read_text()
        reset = re.search(r'^int virtio_transport_reset\([^;{}]*\)\s*\{.*?^\}',
                          transport, re.M | re.S)
        self.assertIsNotNone(reset)
        model = (ROOT / 'tools/tests/virtio_snd_lifecycle_host.c').read_text()
        model = model.replace('/* PRODUCTION_RESET */', reset.group())
        model = model.replace('/* PRODUCTION_SOUND */', code)
        with tempfile.TemporaryDirectory(prefix='a20-virtio-snd-') as folder:
            c, exe = Path(folder) / 'check.c', Path(folder) / 'check'
            c.write_text(model)
            compiled = subprocess.run([
                'cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=address,undefined', '-fno-omit-frame-pointer', '-no-pie',
                '-I' + str(ROOT / 'kernel/include'), str(c), '-o', str(exe),
            ], capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            subprocess.run([str(exe)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

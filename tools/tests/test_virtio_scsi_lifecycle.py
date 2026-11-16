"""Run production SCSI commands/probe/stop with late-DMA and reset-failure models."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    # Restrict the return type and reject declarations/comments mentioning a
    # function before its definition. Keep each production body unchanged.
    match = re.search(r'^(?:static )?(?:int|void|uint32_t|block_dev_t)\s+\*?' +
                      name + r'\([^;{}]*\)\s*\{.*?^\}', source, re.M | re.S)
    if not match:
        raise AssertionError(f'missing production function {name}')
    return match.group()


class VirtioScsiLifecycleTests(unittest.TestCase):
    def test_timeout_keeps_dma_private_and_stop_retains_failed_probe(self):
        source = (ROOT / 'kernel/drivers/block/virtio_scsi.c').read_text()
        header = (ROOT / 'kernel/include/drivers/block/virtio_blk.h').read_text()
        transport = (ROOT / 'kernel/include/drivers/bus/virtio_transport.h').read_text()
        constants = '\n'.join(re.findall(r'^#define (?:VIRTIO_|VIRTQ_).*$', header, re.M))
        queues = header[header.index('/* VirtIO descriptor */'):header.index('/* VirtIO block request header */')]
        types = source[source.index('#define VIRTIO_SCSI_QUEUE_SIZE'):source.index('/* a20.virtio-scsi.poll=1')]
        transport = transport[transport.index('typedef struct virtio_transport'):transport.index('/* Task context')]
        definitions = constants + '\n' + queues + transport + types
        implementations = re.sub(r'^static int virtio_scsi_stop\(void \*context\);\n', '', source, flags=re.M)
        names = ('be32', 'put_be32', 'virtio_scsi_setup_queue', 'virtio_scsi_setup_queues',
                 'virtio_scsi_was_unit_attention', 'virtio_scsi_command_once',
                 'virtio_scsi_command', 'virtio_scsi_rw', 'virtio_scsi_read',
                 'virtio_scsi_write', 'virtio_scsi_flush', 'virtio_scsi_block_release',
                 'virtio_scsi_probe', 'virtio_scsi_stop', 'virtio_scsi_remove',
                 'virtio_scsi_get_dev', 'virtio_scsi_ready')
        reset = function((ROOT / 'kernel/drivers/bus/virtio_transport.c').read_text(), 'virtio_transport_reset')
        generated = (ROOT / 'tools/tests/virtio_scsi_lifecycle_host.c').read_text()
        generated = generated.replace('/* PRODUCTION_DEFINITIONS */', definitions)
        generated = generated.replace('/* PRODUCTION_FUNCTIONS */', reset + '\n' + '\n'.join(function(implementations, n) for n in names))
        with tempfile.TemporaryDirectory(prefix='a20-scsi-lifecycle-') as temp:
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

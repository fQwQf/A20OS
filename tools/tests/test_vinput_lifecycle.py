"""Production VirtIO input and reset helper with modeled transport/IRQ/core."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class VinputLifecycle(unittest.TestCase):
    def test_dma_layout_shared_irq_and_retained_reset(self):
        code = (ROOT / 'tools/tests/vinput_lifecycle_host.c').read_text()
        files = [('TRANSPORT', 'kernel/include/drivers/bus/virtio_transport.h'),
                 ('EVENTS', 'kernel/include/drivers/input/virtio_input.h'),
                 ('INPUT', 'kernel/include/core/input.h'),
                 ('EVDEV', 'kernel/include/drivers/input/evdev.h'),
                 ('RESET', 'kernel/drivers/bus/virtio_transport.c'),
                 ('PRODUCTION', 'kernel/drvmod/examples/vinput.c')]
        for marker, name in files:
            source = re.sub(r'^#include[^\n]*\n', '', (ROOT / name).read_text(), flags=re.M)
            code = code.replace('/* ' + marker + ' */', source)
        registers = (ROOT / 'kernel/include/drivers/dual/virtio_mmio.h').read_text()
        code = code.replace('/* REGISTERS */', '\n'.join(
            line for line in registers.splitlines()
            if line.startswith(('#define VMMIO_', '#define VIRTIO_STATUS_'))))
        with tempfile.TemporaryDirectory(prefix='a20-vinput-') as folder:
            path = Path(folder) / 'input.c'
            path.write_text(code)
            binary = path.with_suffix('')
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            '-fno-omit-frame-pointer', '-fno-pie', '-no-pie',
                            str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=10)


if __name__ == '__main__':
    unittest.main()

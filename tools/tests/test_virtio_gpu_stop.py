"""Real transport reset and all four GPU command owners under teardown/failure."""
from pathlib import Path
import subprocess
import re
import tempfile
import unittest
from .test_vmo_contiguous import build_fixture, ROOT
from .test_drv_dma_phys_host import extract_function

class VirtioGpuStop(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-virtio-gpu-stop-');cls.addClassCleanup(cls.temp.cleanup)
        source=(ROOT/'kernel/drivers/gpu/virtio_gpu.c').read_text()
        extra=(ROOT/'tools/tests/virtio_gpu_stop_host.c').read_text()
        extra=extra.replace('/* PRODUCTION_INSTANCE */',source[source.index('typedef struct {'):source.index('} virtio_gpu_inst_t;')+len('} virtio_gpu_inst_t;')])
        extra=extra.replace('/* PRODUCTION_CLAIM */',source[source.index('static virtio_gpu_inst_t g_gpu_inst;'):source.index('/* The host copies a SUBMIT_3D')])
        extra=extra.replace('/* PRODUCTION_RESET */',extract_function((ROOT/'kernel/drivers/bus/virtio_transport.c').read_text(),'int virtio_transport_reset('))
        def body(name):
            signature=re.search(re.escape(name.split('(')[0]+'(')+r'[^;{]*\{',source).group()[:-1]
            return extract_function(source,signature)
        names=['static int virtio_gpu_send_cmd(', 'static int virtio_gpu_send_cmd_big(', 'static int gpu_flush(', 'static int virtio_gpu_submit_3d(', 'static int virtio_gpu_resource_attach_backing(']
        extra=extra.replace('/* PRODUCTION_COMMANDS */','\n'.join(body(n) for n in names))
        names=['static int virtio_gpu_init_transport(', 'static int virtio_gpu_probe(', 'static void virtio_gpu_release_buffers(virtio_gpu_inst_t *inst)\n{','static void virtio_gpu_quiesce(', 'static int virtio_gpu_remove(']
        extra=extra.replace('/* PRODUCTION_LIFETIME */','\n'.join(body(n) for n in names))
        cls.binary=build_fixture(Path(cls.temp.name),suffix=extra)
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_reset_rejects_parameters_before_io(self):self.run_case('reset-parameters')
    def test_reset_requires_zero_readback(self):self.run_case('reset-delayed')
    def test_reset_timeout_and_retry(self):self.run_case('reset-deadline')
    def test_reset_stalled_or_absent_clock_has_poll_bound(self):self.run_case('reset-stalled')
    def test_reset_clock_wrap_and_high_frequency(self):self.run_case('reset-wrap')
    def test_probe_transport_failure_retains_native_cleanup_owner(self):self.run_case('probe-pci-fail')
    def test_initial_reset_failure_blocks_replacement_and_retains_owner(self):self.run_case('probe-reset-fail')
    def test_partial_probe_storage_waits_for_reset_ack(self):self.run_case('probe-partial')
    def test_remove_failure_retains_irq_staging_and_ram_owner(self):self.run_case('remove-retained')
    def test_recursive_active_command_blocks_remove(self):self.run_case('remove-active')
    def test_all_four_queues_complete(self):
        for path in range(4):
            with self.subTest(path=path):self.run_case('command-success-'+str(path))
    def test_all_four_timeouts_quarantine_shared_staging(self):
        for path in range(4):
            with self.subTest(path=path):self.run_case('command-timeout-'+str(path))
    def test_all_four_wrong_completions_quarantine_shared_staging(self):
        for path in range(4):
            with self.subTest(path=path):self.run_case('command-wrong-id-'+str(path))
    def test_all_four_unexpected_index_advances_quarantine_shared_staging(self):
        for path in range(4):
            with self.subTest(path=path):self.run_case('command-wrong-index-'+str(path))
    def test_failed_framebuffer_allocation_retains_irq_until_reset(self):self.run_case('probe-vmo-oom')
    def test_staging_allocation_failures_do_not_publish_or_poison_queue(self):
        for path in range(1,4):
            with self.subTest(path=path):self.run_case('command-oom-'+str(path))

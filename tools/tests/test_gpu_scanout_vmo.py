"""Owning native RAM scanouts and fbdev's VMO map/reference transfer."""
from pathlib import Path
import tempfile
import subprocess
import unittest
from .test_vmo_contiguous import build_fixture, ROOT
from .test_drv_dma_phys_host import extract_function

class GpuScanoutVmo(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-gpu-scanout-vmo-');cls.addClassCleanup(cls.temp.cleanup)
        extra=(ROOT/'tools/tests/gpu_scanout_vmo_host.c').read_text()
        core=(ROOT/'kernel/drivers/gpu/gpu_core.c').read_text()
        body='\n'.join(extract_function(core,n) for n in ['void gpu_scanout_put(', 'int gpu_device_scanout_get('])
        extra=extra.replace('/* PRODUCTION_SNAPSHOT */',body)
        extra=extra.replace('/* PRODUCTION_FB_SCANOUT */',extract_function((ROOT/'kernel/drivers/gpu/framebuffer.c').read_text(),'static int64_t fb_map_scanout('))
        extra=extra.replace('/* PRODUCTION_MAP_CAP */',extract_function((ROOT/'kernel/mm/mmap.c').read_text(),'vaddr_t mm_mmap_vmo_cap('))
        virtio=(ROOT/'kernel/drivers/gpu/virtio_gpu.c').read_text()
        body='\n'.join(extract_function(virtio,n) for n in ['static int gpu_get_scanout(',
            'static void virtio_gpu_release_buffers(virtio_gpu_inst_t *inst)\n{', 'static int virtio_gpu_queue_stop(void *context)\n{', 'static int virtio_gpu_remove('])
        extra=extra.replace('/* PRODUCTION_VIRTIO */',body)
        cls.binary=build_fixture(Path(cls.temp.name),suffix=extra)
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_arguments_missing_provider_and_idempotent_put(self):self.run_case('arguments')
    def test_invalid_or_partial_snapshots_return_provider_reference(self):self.run_case('invalid')
    def test_snapshot_survives_provider_and_operation_table_destruction(self):self.run_case('detached')
    def test_fb_mapping_retains_ram_after_provider_reference_drop(self):self.run_case('map')
    def test_fb_validation_and_failed_map_return_temporary_references(self):self.run_case('map-failures')

    def test_real_virtio_get_and_remove_transfer_storage_outside_module(self):self.run_case('virtio-provider')

    def test_initial_protection_fits_immutable_ceiling_before_mm_transaction(self):self.run_case('cap-validation')

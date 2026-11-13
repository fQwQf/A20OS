"""Native DMA buffer lifetime with production allocator bodies and driver core."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE
from .test_usb_concurrency import LOCKS
from .test_drv_dma_phys_host import extract_function


def native_buffer_source():
    production = (ROOT / 'kernel/drivers/core/driver_hwapi.c').read_text()
    start = production.index('struct driver_dma_buffer {')
    shape = production[start:production.index('};', start)+2]
    bodies = '\n'.join(extract_function(production, name) for name in (
        'static int dma_mask_well_formed(', 'static int dma_arch_ready(',
        'uint64_t dma_get_mask(', 'static int dma_range_ok_mask(', 'static int dma_page_order(size_t size, size_t alignment)\n{',
        'int driver_dma_buffer_create(', 'void driver_dma_buffer_get(',
        'void driver_dma_buffer_put(', 'void *driver_dma_buffer_data(',
        'uint64_t driver_dma_buffer_address(', 'size_t driver_dma_buffer_size('))
    model = (ROOT / 'tools/tests/driver_dma_buffer_host.c').read_text().replace('/* PRODUCTION_BUFFER */', shape+'\n'+bodies)
    source = LOCKS + C_FIXTURE[C_FIXTURE.index('#include "drivers/core/driver_core.c"'):]
    source = source.replace('struct task_t *proc_current(void) {return NULL;}',
                            '_Thread_local char buffer_task;\nstruct task_t *proc_current(void) {return (void *)&buffer_task;}')
    marker = 'int main(int argc,char **argv) {'
    source = source.replace(marker, model+'\n'+marker)
    source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                            'if(!strncmp(argv[1],"buffer-",7))dma_buffer_case(argv[1]+7);\n else if(!strcmp(argv[1],"pci-retired"))')
    return source


class DriverDmaBuffer(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-dma-buffer-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = native_buffer_source()
        path = work / 'fixture.c';path.write_text(source);cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DCONFIG_X86_64', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch/x86_64/include'), '-I'+str(ROOT / 'kernel/include'),
                        '-I'+str(ROOT / 'kernel'), str(path), '-o', str(cls.binary)], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), 'buffer-'+name], check=True, timeout=15)

    def test_parameters_architecture_and_owner_admission(self): self.run_case('parameters')
    def test_failure_unwinds_allocation_and_device_pin(self): self.run_case('allocation')
    def test_owner_and_mask_changes_during_allocation_are_rejected(self): self.run_case('races')
    def test_active_class_allocation_does_not_wait_on_lifecycle(self): self.run_case('class-entry')
    def test_allocator_limit_and_large_alignment(self):
        self.run_case('maximum'); self.run_case('zero')
    def test_small_request_strong_alignment_outlives_owner_registration(self): self.run_case('lifetime')
    def test_reference_handoffs_preserve_single_final_free(self): self.run_case('references')

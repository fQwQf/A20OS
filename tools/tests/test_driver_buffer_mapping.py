"""Real buffer allocation owners, core and page mappings across failed invalidation."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT
from .test_driver_dma_buffer import native_buffer_source


class DriverBufferMapping(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-buffer-map-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = native_buffer_source()
        # Keep the weak readiness definition after both production users.
        marker = 'int arch_device_dma_ready(void) {return dma_ready;}'
        source = source.replace(marker, '#include "drivers/core/dma_policy.c"\n'
                                '#include "drivers/core/dma_mapping.c"\n'+marker)
        model = (ROOT / 'tools/tests/driver_buffer_mapping_host.c').read_text()
        source = source.replace('int main(int argc,char **argv) {', model+'\nint main(int argc,char **argv) {')
        source = source.replace('if(!strncmp(argv[1],"buffer-",7))',
                                'if(!strncmp(argv[1],"map-",4))buffer_mapping_case(argv[1]+4);\n else if(!strncmp(argv[1],"buffer-",7))')
        fixture = work / 'fixture.c';fixture.write_text(source);cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DCONFIG_X86_64', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch/x86_64/include'), '-I'+str(ROOT / 'kernel/include'),
                        '-I'+str(ROOT / 'kernel'), str(fixture), '-o', str(cls.binary)], check=True)

    def run_case(self, name): subprocess.run([str(self.binary), 'map-'+name], check=True, timeout=15)

    def test_nonidentity_mapping_outlives_independent_allocation_owner(self): self.run_case('normal')
    def test_strong_alignment_backing_is_not_all_exposed(self):
        self.run_case('padding')
        self.run_case('excluded')
    def test_device_access_directions_preserve_cache_handoffs(self):
        self.run_case('to-device')
        self.run_case('from-device')
    def test_uncertain_map_rollback_keeps_entire_allocation(self): self.run_case('rollback')
    def test_partial_unmap_failure_retains_storage_and_defers_cpu_sync(self): self.run_case('unmap')
    def test_policy_checks_padding_before_any_translation_or_pin(self): self.run_case('preflight')
    def test_identity_policy_violation_retains_cleanup_then_retries(self): self.run_case('identity')
    def test_invalid_admission_oom_and_provider_errors_balance_references(self): self.run_case('errors')
    def test_mapping_and_shutdown_serialize_before_closing_admission(self): self.run_case('shutdown')

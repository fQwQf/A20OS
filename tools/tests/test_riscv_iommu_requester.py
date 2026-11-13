"""Production RISC-V DMA admission and cleanup share the PCI requester ledger."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest

from .test_native_bus_lifetime import ROOT, C_FIXTURE
from .test_pci_requester import LOCKS


class RiscvIommuRequester(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86_64 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-iommu-requester-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        source = LOCKS + C_FIXTURE[C_FIXTURE.index('#include "drivers/core/driver_core.c"'):]
        source = source.replace('struct task_t *proc_current(void) {return NULL;}',
                                'static _Thread_local char task;\nstruct task_t *proc_current(void) {return (struct task_t *)&task;}')
        config_write = 'void arch_pci_config_write32(int bus,int dev,int fn,uint32_t reg,uint32_t value) {'
        assert source.count(config_write) == 1
        source = source.replace(config_write, 'static int reject_user_bme;\n' + config_write + '\n if(reg==4&&reject_user_bme)return;')
        marker = 'int main(int argc,char **argv) {'
        source = source.replace(marker, (ROOT / 'tools/tests/riscv_iommu_requester_host.c').read_text() + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"iommu-",6))requester_iommu_case(argv[1]+6);\n else if(!strcmp(argv[1],"pci-retired"))')
        path = work / 'fixture.c'
        path.write_text(source)
        cls.binary = work / 'test'
        subprocess.run([
            'cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
            '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
            '-DCONFIG_X86_64', '-DCONFIG_IOMMU_TRRESP_LEGACY_PPN', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
            '-I' + str(ROOT / 'kernel/arch/x86_64/include'),
            '-I' + str(ROOT / 'kernel/include'), '-I' + str(ROOT / 'kernel'),
            str(path), '-o', str(cls.binary),
        ], check=True)

    def run_cases(self, *names):
        for name in names:
            with self.subTest(case=name):
                subprocess.run([str(self.binary), 'iommu-' + name], check=True, timeout=15)

    def test_failed_probe_retains_real_domain_frames_and_lease_until_safe_cleanup(self):
        self.run_cases('probe-cleanup', 'probe-child')

    def test_native_and_user_claims_conflict_with_external_leases(self):
        self.run_cases('external', 'user')

    def test_allocation_failures_publish_no_partial_claim(self):
        self.run_cases('lease-allocation', 'native-allocations', 'pages')

    def test_confirmed_failed_creation_releases_lease_and_tables(self):
        self.run_cases('safe-failure', 'user-bme-read', 'user-bme-write')

    def test_uncertain_failed_claim_retains_lease_through_retry(self):
        self.run_cases('failed-claim', 'native-failed-claim')

    def test_failed_unmap_and_detach_preserve_frames_and_requester(self):
        self.run_cases('failed-release')

    def test_shutdown_avoids_discovery_and_managed_mutex_inversion(self):
        self.run_cases('lock-order')

    def test_competing_provider_and_external_leases_with_real_threads(self):
        self.run_cases('concurrent')

    def test_replacement_and_removal_do_not_refresh_or_drop_old_claims(self):
        self.run_cases('replacement', 'removed')


if __name__ == '__main__':
    unittest.main()

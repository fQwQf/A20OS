"""Verified controller stop must precede real core/USB consumer destruction."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE
from .test_xhci_firmware_policy import function


class XhciRemove(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-xhci-remove-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        production = (ROOT / 'kernel/drivers/usb/host/xhci.c').read_text()
        bodies = '\n'.join(function(production, name) for name in (
            'xhci_wait32', 'xhci_xfer_acquire', 'xhci_xfer_release',
            'xhci_prepare_remove', 'xhci_op_abort_slot', 'xhci_remove'))
        # Release normally clears a hardware deadline; the rest is production.
        model = (ROOT / 'tools/tests/xhci_remove_host.c').read_text().replace('/* PRODUCTION_FUNCTIONS */', bodies)
        marker = 'int main(int argc,char **argv) {'
        source = C_FIXTURE.replace(marker, model + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"stop-",5))stop_case(argv[1]+5);\n else if(!strcmp(argv[1],"pci-retired"))')
        fixture = work / 'fixture.c'
        fixture.write_text(source)
        cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DCONFIG_X86_64', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch/x86_64/include'),
                        '-I'+str(ROOT / 'kernel/include'), '-I'+str(ROOT / 'kernel'),
                        str(fixture), '-o', str(cls.binary)], check=True)

    def run_cases(self, *cases):
        for case in cases:
            with self.subTest(case=case):
                subprocess.run([str(self.binary), 'stop-'+case], check=True, timeout=10)

    def test_failed_mmio_reads_and_run_write_keep_consumers_and_rings(self):
        self.run_cases('command-read', 'status-read', 'command-write')

    def test_halt_reset_and_ready_waits_must_complete_before_cleanup(self):
        self.run_cases('halt', 'reset', 'not-ready')

    def test_bus_master_read_and_clear_are_checked(self):
        self.run_cases('pci-read', 'pci-write')

    def test_busy_transfer_keeps_graph_without_waiting_on_lifecycle(self):
        self.run_cases('busy')

    def test_failed_probe_retains_controller_until_stop_retry(self):
        self.run_cases('failed-probe', 'unowned')

    def test_confirmed_stop_reclaims_known_slots_without_new_commands(self):
        self.run_cases('success')

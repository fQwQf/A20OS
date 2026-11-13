"""Failed PCI and USB removal retains transport identity and backing storage."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE


class NativeBusRemove(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        machine = platform.machine().lower()
        if machine in ('x86_64', 'amd64'):
            arch, macro = 'x86_64', 'CONFIG_X86_64'
        elif machine in ('aarch64', 'arm64'):
            arch, macro = 'aarch64', 'CONFIG_AARCH64'
        else:
            raise unittest.SkipTest('host architecture headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-bus-remove-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        fixture = work / 'fixture.c'
        marker = 'int main(int argc,char **argv) {'
        source = C_FIXTURE.replace(marker, (ROOT / 'tools/tests/native_bus_remove_host.c').read_text() + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"remove-",7))native_remove_case(argv[1]+7);\n else if(!strcmp(argv[1],"pci-retired"))')
        fixture.write_text(source)
        cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-D'+macro, '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch' / arch / 'include'),
                        '-I'+str(ROOT / 'kernel/include'), '-I'+str(ROOT / 'kernel'),
                        str(fixture), '-o', str(cls.binary)], check=True)

    def run_case(self, case):
        subprocess.run([str(self.binary), 'remove-'+case], check=True, timeout=10)

    def test_pci_replacement_waits_for_old_binding_to_stop(self):
        self.run_case('pci-replace')

    def test_missing_pci_function_keeps_slot_and_resources_on_stop_failure(self):
        self.run_case('pci-missing')

    def test_hcd_failure_retains_ports_interfaces_endpoints_and_address(self):
        self.run_case('usb-hcd')

    def test_already_unregistered_interface_is_released_by_its_publisher(self):
        self.run_case('usb-already')

    def test_failed_slot_abort_keeps_every_interface_and_retries_stop(self):
        self.run_case('usb-abort')
        self.run_case('usb-no-abort')

    def test_port_removal_is_retried_before_enumerating_replacement(self):
        self.run_case('usb-port')


if __name__ == '__main__':
    unittest.main()

"""Partial USB enumeration keeps the port owner until fallible slot teardown."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE


class UsbEnumerationCleanup(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        machine = platform.machine().lower()
        if machine in ('x86_64', 'amd64'):
            arch, macro = 'x86_64', 'CONFIG_X86_64'
        elif machine in ('aarch64', 'arm64'):
            arch, macro = 'aarch64', 'CONFIG_AARCH64'
        else:
            raise unittest.SkipTest('host architecture headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-usb-setup-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        marker = 'int main(int argc,char **argv) {'
        source = C_FIXTURE.replace(marker, (ROOT / 'tools/tests/usb_enumeration_cleanup_host.c').read_text() + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"setup-",6))usb_setup_cleanup_case(argv[1]+6);\n else if(!strcmp(argv[1],"pci-retired"))')
        fixture = work / 'fixture.c'
        fixture.write_text(source)
        cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu99', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-D'+macro, '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch' / arch / 'include'),
                        '-I'+str(ROOT / 'kernel/include'), '-I'+str(ROOT / 'kernel'),
                        str(fixture), '-o', str(cls.binary)], check=True)

    def run_cases(self, *cases):
        for case in cases:
            with self.subTest(case=case):
                subprocess.run([str(self.binary), 'setup-'+case], check=True, timeout=10)

    def test_failed_slot_allocation_hands_off_partial_context(self):
        self.run_cases('slot', 'implicit', 'positive', 'stop-positive', 'safe-failure', 'empty-slot', 'invalid-slot')

    def test_descriptor_and_config_failures_retain_slot_until_stopped(self):
        self.run_cases('descriptor-1', 'descriptor-2', 'descriptor-3', 'descriptor-4', 'control', 'parse', 'mps')

    def test_allocation_failure_after_slot_needs_no_new_cleanup_allocation(self):
        self.run_cases('oom-1', 'oom-2', 'oom-3')

    def test_missing_abort_callback_preserves_partial_owner(self):
        self.run_cases('no-abort')

    def test_cleanup_retries_do_not_reset_the_per_connection_attempt_budget(self):
        self.run_cases('budget')

    def test_partial_publication_stops_slot_before_bound_interface_rollback(self):
        self.run_cases('publication')

    def test_implicit_address_never_releases_another_controllers_reservation(self):
        self.run_cases('implicit-other', 'implicit-success')

    def test_preflight_and_owner_oom_never_publish_a_slot(self):
        self.run_cases('invalid', 'owner-oom', 'pool')


if __name__ == '__main__':
    unittest.main()

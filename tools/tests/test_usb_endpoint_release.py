"""Native interface release runs production core/HID/xHCI under host faults."""
from pathlib import Path
import platform
import re
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE
from .test_usb_concurrency import LOCKS
from .test_xhci_firmware_policy import function


class UsbEndpointRelease(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-usb-release-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        model = (ROOT / 'tools/tests/usb_endpoint_release_host.c').read_text()
        production = (ROOT / 'kernel/drivers/usb/host/xhci.c').read_text()
        bodies = '\n'.join(function(production, name) for name in (
            'xhci_find_ep', 'xhci_ring_init', 'xhci_ring_enqueue',
            'xhci_xfer_acquire', 'xhci_xfer_release', 'xhci_collect_interrupt',
            'xhci_run_completions', 'xhci_op_configure_endpoint',
            'xhci_op_release_endpoint', 'xhci_op_submit_interrupt'))
        model = model.replace('/* PRODUCTION_FUNCTIONS */', bodies)
        hid = (ROOT / 'kernel/drivers/usb/class/usb_hid.c').read_text()
        defines = '\n'.join(re.findall(r'^#define (?:USB_HID_|VBOX_USB_|ITE_)[^\n]+', hid, re.M))
        shape = hid[hid.index('typedef struct usb_hid_dev'):hid.index('} usb_hid_dev_t;')+len('} usb_hid_dev_t;')]
        model = model.replace('/* HID_TYPES */', '#include "drivers/input/virtio_input.h"\n'+defines+'\n'+shape.replace('ALIGNED(64)', ''))
        model = model.replace('/* HID_FUNCTIONS */', '\n'.join(function(hid, name) for name in (
            'usb_hid_probe', 'usb_hid_quiesce', 'usb_hid_remove')))
        hub = (ROOT / 'kernel/drivers/usb/class/usb_hub.c').read_text()
        model = model.replace('/* HUB_FUNCTIONS */', function(hub, 'usb_hub_remove'))
        source = LOCKS + C_FIXTURE[C_FIXTURE.index('#include "drivers/core/driver_core.c"'):]
        marker = 'int main(int argc,char **argv) {'
        source = source.replace(marker, model + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"endpoint-",9))endpoint_case(argv[1]+9);\n else if(!strcmp(argv[1],"pci-retired"))')
        fixture = work / 'fixture.c'
        fixture.write_text(source)
        cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DCONFIG_X86_64', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch/x86_64/include'),
                        '-I'+str(ROOT / 'kernel/include'), '-I'+str(ROOT / 'kernel'),
                        str(fixture), '-o', str(cls.binary)], check=True)

    def run_cases(self, *cases):
        for case in cases:
            with self.subTest(case=case):
                subprocess.run([str(self.binary), 'endpoint-'+case], check=True, timeout=15)

    def test_partial_probe_and_independent_hid_unbind_retain_callback_ownership(self):
        self.run_cases('probe-configure', 'probe-submit', 'probe-rebind', 'probe-interface')

    def test_stop_drop_checkpoints_deny_rearm_and_make_retry_idempotent(self):
        self.run_cases('retry')

    def test_unlocked_callback_keeps_endpoint_and_interface_storage_until_retry(self):
        self.run_cases('callback')

    def test_other_endpoint_and_its_callback_remain_usable(self):
        self.run_cases('siblings')

    def test_halted_and_error_endpoint_use_their_specified_stop_transitions(self):
        self.run_cases('states')

    def test_successful_command_requires_stopped_and_disabled_context_readback(self):
        self.run_cases('readback')

    def test_configure_publishes_cleanup_owner_before_command_and_refuses_duplicates(self):
        self.run_cases('configure')

    def test_interface_release_requires_current_stopping_owner(self):
        self.run_cases('admission', 'busy-fatal', 'partial-interface')

    def test_hub_retains_owner_when_upstream_release_fails_after_downstream_exit(self):
        self.run_cases('hub-partial')

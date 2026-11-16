"""Real unlocked completion callbacks race production prepare and slot abort."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT, C_FIXTURE
from .test_usb_concurrency import LOCKS
from .test_xhci_firmware_policy import function


class XhciCallbackLifetime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-xhci-callback-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        model = (ROOT / 'tools/tests/xhci_remove_host.c').read_text()
        model = model.replace('!stop_controller->lock.held', 'true').replace('!x->lock.held', 'true')
        model = model.replace('#define XHCI_WAIT_LOOPS 4U', '#define XHCI_WAIT_LOOPS 4U\n#define XHCI_IRQ_BATCH 8U\n#define XHCI_TRB_TRANSFER_EVENT 32U')
        model = model.replace('uint64_t xfer_deadline,dcbaa[3];', 'uint8_t control_buffer[4096];uint64_t xfer_deadline,dcbaa[3];')
        helper = r'''
typedef struct {uint64_t setup,data,bulk_first;} xhci_transfer_trbs_t;
typedef struct {uint64_t parameter;uint32_t status,control;} xhci_trb_t;
static unsigned control_posts;
static uint64_t xhci_control_locked(xhci_controller_t *x,uint8_t slot,const usb_setup_packet_t *s,void *data,xhci_transfer_trbs_t *c) {
 (void)x;(void)s;(void)data;(void)c;assert(slot==1);control_posts++;return 5;
}
static int xhci_wait_event(xhci_controller_t *x,uint8_t type,uint64_t pointer,xhci_trb_t *event,const xhci_transfer_trbs_t *c) {
 (void)x;(void)event;(void)c;assert(type==32&&pointer==5);return 0;
}
'''
        production = (ROOT / 'kernel/drivers/usb/host/xhci.c').read_text()
        bodies = '\n'.join(function(production, name) for name in (
            'xhci_find_ep', 'xhci_wait32', 'xhci_xfer_acquire', 'xhci_xfer_release', 'xhci_run_completions',
            'xhci_prepare_remove', 'xhci_op_abort_slot', 'xhci_remove', 'xhci_control'))
        model = model.replace('/* PRODUCTION_FUNCTIONS */', helper + bodies)
        model += (ROOT / 'tools/tests/xhci_callback_host.c').read_text()
        marker = 'int main(int argc,char **argv) {'
        source = LOCKS + C_FIXTURE[C_FIXTURE.index('#include "drivers/core/driver_core.c"'):]
        source = source.replace(marker, model + '\n' + marker)
        source = source.replace('if(!strcmp(argv[1],"pci-retired"))',
                                'if(!strncmp(argv[1],"callback-",9))callback_case(argv[1]+9);\n else if(!strncmp(argv[1],"stop-",5))stop_case(argv[1]+5);\n else if(!strcmp(argv[1],"pci-retired"))')
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
                subprocess.run([str(self.binary), 'callback-'+case], check=True, timeout=10)

    def test_controller_preparation_keeps_unlocked_and_reentrant_callbacks_alive(self):
        self.run_cases('prepare', 'reentrant')

    def test_slot_stop_waits_for_its_callback_without_repeating_disable(self):
        self.run_cases('abort')

    def test_another_slots_callback_does_not_block_confirmed_slot_cleanup(self):
        self.run_cases('unrelated')

    def test_retirement_does_not_dereference_a_self_freed_urb(self):
        self.run_cases('self-free')

    def test_stopped_slot_refuses_control_without_posting(self):
        self.run_cases('control')

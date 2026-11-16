"""Production bulk staging and allocation owners survive uncertain DMA stop."""
from pathlib import Path
import platform
import subprocess
import tempfile
import unittest
from .test_native_bus_lifetime import ROOT
from .test_driver_dma_buffer import native_buffer_source
from .test_xhci_firmware_policy import function


class XhciBulkLifetime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if platform.machine().lower() not in ('x86_64', 'amd64'):
            raise unittest.SkipTest('x86 hardware headers required')
        cls.temp = tempfile.TemporaryDirectory(prefix='a20-xhci-bulk-')
        cls.addClassCleanup(cls.temp.cleanup)
        work = Path(cls.temp.name)
        model = (ROOT / 'tools/tests/xhci_remove_host.c').read_text()
        model = model.replace('void driver_dma_buffer_put(struct driver_dma_buffer *buffer) {assert(!buffer);}\n', '')
        model = model.replace('!stop_controller->lock.held', 'true').replace('!x->lock.held', 'true')
        shapes = r'''
#define XHCI_RING_TRBS 64U
#define XHCI_BULK_MAX_LENGTH 65536U
#define XHCI_TRB_LINK 6U
#define XHCI_TRB_ENT 2U
#define XHCI_TRB_CYCLE 1U
#define XHCI_TRB_CHAIN 16U
#define XHCI_CC_SUCCESS 1U
#define XHCI_CC_SHORT_PACKET 13U
#define XHCI_TRB_NORMAL 1U
#define XHCI_TRB_ISP 4U
#define XHCI_TRB_IOC 32U
#define XHCI_TRB_TRANSFER_EVENT 32U
typedef struct {uint64_t parameter;uint32_t status,control;} xhci_trb_t;
typedef struct {xhci_trb_t trbs[XHCI_RING_TRBS];uint16_t enqueue;uint8_t cycle;} xhci_ring_t;
typedef struct {uint64_t setup,data,bulk_first;} xhci_transfer_trbs_t;
static int xhci_event_matches(const xhci_trb_t *,uint8_t,uint64_t,const xhci_transfer_trbs_t *);
static unsigned bulk_bells,bulk_waits;static int wait_rc,sync_race,wait_early;static uint32_t wait_remaining,wait_cc;static void *bulk_caller;
#undef wmb
#define wmb() ((void)0)
static uint64_t bulk_address(const void *p) {return (uintptr_t)p;}
#define va_to_pa bulk_address
'''
        model = shapes + model
        model = model.replace('uint8_t slot,dci,closing,stopped,disabled;', 'xhci_ring_t ring;uint8_t addr,slot,dci,closing,stopped,disabled;')
        model = model.replace('uintptr_t op,runtime;', 'uintptr_t op,runtime,doorbell;')
        model = model.replace(' register_reads++;', ' register_reads++;\n if(base==0x3000){assert(reg==4);return 0;}')
        model = model.replace(' register_writes++;', ' register_writes++;\n if(base==0x3000){assert(reg==4&&value);bulk_bells++;return;}')
        model = model.replace('static void stop_sync(const void *p,size_t n) {assert(p&&n);}', '''static void stop_sync(const void *p,size_t n) {
 assert(p&&n);if(p==small_memory&&sync_race){if(sync_race==1)stop_controller->fatal=1;else stop_controller->eps->closing=1;}
}''')
        model = model.replace('#define arch_dma_sync_for_device stop_sync', '#define arch_dma_sync_for_device stop_sync\n#define arch_dma_sync_for_cpu stop_sync')
        helper = r'''
static int xhci_wait_event(xhci_controller_t *x,uint8_t type,uint64_t pointer,xhci_trb_t *event,const xhci_transfer_trbs_t *scope) {
 assert(x->xfer_busy&&type==32&&pointer&&event&&scope);bulk_waits++;
 xhci_trb_t *last=(void *)(uintptr_t)pointer;
 xhci_trb_t *first=scope->bulk_first?(void *)(uintptr_t)scope->bulk_first:last;
 uint64_t base=driver_dma_buffer_address(x->bulk_buffer);
 assert(first->parameter==base&&base!=(uintptr_t)bulk_caller);
 unsigned initial=first->status&0x1ffffU,tail=first==last?0:last->status&0x1ffffU;
 assert((base&65535U)+initial<=65536U);
 if(first!=last){
  assert(last->parameter==base+initial&&!(last->parameter&65535U));
  assert((first->control&XHCI_TRB_CHAIN)&&!(first->control&XHCI_TRB_IOC));
  assert(!(last->control&XHCI_TRB_CHAIN)&&(last->control&XHCI_TRB_IOC));
  assert(((first->status>>17)&31U)==(tail/512>31?31:tail/512));
 }
 unsigned total=initial+tail,limit=wait_early?initial:total;
 if(!wait_rc&&wait_remaining<=limit){
  if(x->hcd.port_devices[0]->ifaces[0].eps[0].addr&USB_DIR_IN)
   memset(small_memory,0x3c,limit-wait_remaining);
  else if(total)assert(small_memory[0]==0x55&&small_memory[total-1]==0x55);
 }
 event->parameter=wait_early?(uintptr_t)first:pointer;
 event->control=XHCI_TRB_TYPE(32);
 event->status=wait_remaining|((wait_cc?wait_cc:1U)<<24);
 xhci_trb_t unrelated=*event;unrelated.parameter=0;unrelated.status=6U<<24;
 assert(!xhci_event_matches(&unrelated,32,pointer,scope));
 assert(xhci_event_matches(event,32,pointer,scope));
 if(wait_rc==-ETIMEDOUT){x->fatal=1;x->running=0;}
 return wait_rc;
}
'''
        production = (ROOT / 'kernel/drivers/usb/host/xhci.c').read_text()
        bodies = '\n'.join(function(production, name) for name in (
            'xhci_event_matches', 'xhci_find_ep', 'xhci_ring_enqueue', 'xhci_wait32',
            'xhci_xfer_acquire', 'xhci_xfer_release', 'xhci_prepare_remove',
            'xhci_op_abort_slot', 'xhci_remove', 'xhci_op_submit_bulk'))
        model = model.replace('/* PRODUCTION_FUNCTIONS */', helper+bodies)
        model += (ROOT / 'tools/tests/xhci_bulk_host.c').read_text()
        marker = 'int main(int argc,char **argv) {'
        source = native_buffer_source().replace(marker, model+'\n'+marker)
        source = source.replace('if(!strncmp(argv[1],"buffer-",7))',
                                'if(!strncmp(argv[1],"bulk-",5))bulk_case(argv[1]+5);\n else if(!strncmp(argv[1],"stop-",5))stop_case(argv[1]+5);\n else if(!strncmp(argv[1],"buffer-",7))')
        path = work / 'fixture.c';path.write_text(source);cls.binary = work / 'test'
        subprocess.run(['cc', '-std=gnu11', '-pthread', '-Wall', '-Wextra', '-Werror', '-ffreestanding',
                        '-fno-builtin', '-ffunction-sections', '-fdata-sections', '-Wl,--gc-sections',
                        '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                        '-DCONFIG_X86_64', '-DCONFIG_NR_CPUS=1', '-DCONFIG_DRIVER_LIFECYCLE_TEST',
                        '-I'+str(ROOT / 'kernel/arch/x86_64/include'), '-I'+str(ROOT / 'kernel/include'),
                        '-I'+str(ROOT / 'kernel'), str(path), '-o', str(cls.binary)], check=True)

    def run_case(self, name):
        subprocess.run([str(self.binary), 'bulk-'+name], check=True, timeout=15)

    def test_in_out_zero_and_maximum_transfer_use_private_dma(self): self.run_case('copies')
    def test_late_dma_and_failed_halt_keep_private_storage_after_caller_free(self): self.run_case('timeout')
    def test_short_reports_only_transferred_prefix_and_stall_is_distinct(self): self.run_case('short')
    def test_invalid_stopped_busy_and_missing_owner_do_not_submit(self): self.run_case('admission')
    def test_exit_during_copy_is_rechecked_before_doorbell(self): self.run_case('copy-race')
    def test_another_transfer_keeps_sole_staging_ownership(self): self.run_case('busy')
    def test_staging_allocation_failure_unwinds_unstarted_controller(self): self.run_case('probe-failure')
    def test_chained_boundary_wrap_short_and_timeout_retain_whole_td(self): self.run_case('chain')

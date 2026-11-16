"""Production xHCI USB-type translation, contexts and Normal TRB direction.

EP Type values come from xHCI 1.2 table 6-9 (also Linux xhci.h), not the
implementation's old table. HID must remain interrupt IN; BOT must be bulk IN.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    match = re.search(r"^static [^\n]*\b" + name +
                      r"\([^{}]*\)\s*\{.*?^\}", source, re.M | re.S)
    assert match, name
    return match.group()


MODEL = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define USB_DIR_IN 0x80U
#define USB_XFER_CONTROL 0
#define USB_XFER_ISOC 1
#define USB_XFER_BULK 2
#define USB_XFER_INTERRUPT 3
#define USB_SPEED_FULL 1U
#define USB_SPEED_LOW 2U
#define USB_SPEED_HIGH 3U
#define USB_HUB_MAX_PORTS 127U
#define XHCI_MAX_ROUTE_DEPTH 5U
#define XHCI_TRB_CYCLE 1U
#define XHCI_TRB_TYPE(n) ((n)<<10)
#define XHCI_TRB_NORMAL 1U
#define XHCI_TRB_TRANSFER_EVENT 32U
#define XHCI_TRB_ISP (1U<<2)
#define XHCI_TRB_CHAIN (1U<<4)
#define XHCI_TRB_IOC (1U<<5)
#define XHCI_BULK_MAX_LENGTH 65536U
typedef struct{uint64_t parameter;uint32_t status,control;}xhci_trb_t;
typedef struct{uint64_t setup,data,bulk_first;}xhci_transfer_trbs_t;
typedef struct{xhci_trb_t trbs[64];uint16_t enqueue;uint8_t cycle;}xhci_ring_t;
typedef struct{uint8_t addr;uint16_t max_packet;}usb_endpoint_t;
typedef struct{uint8_t slot;}usb_device_t;
typedef struct usb_urb{usb_endpoint_t*ep;usb_device_t*dev;void*buf;size_t len,actual_length;}usb_urb_t;
typedef struct xhci_ep{struct xhci_ep*next;uint8_t slot,dci,closing;xhci_ring_t ring;usb_urb_t*pending;}xhci_ep_t;
typedef struct{int held;}spinlock_t;
typedef struct {
    uint32_t route;
    uint8_t root_port, depth, speed, tt_slot, tt_port;
} xhci_slot_path_t;
struct driver_dma_buffer {unsigned char data[65536];};
static void *driver_dma_buffer_data(const struct driver_dma_buffer *b){return b?(void *)b->data:NULL;}
static uint64_t driver_dma_buffer_address(const struct driver_dma_buffer *b){(void)b;return 0x12340000;}
static size_t driver_dma_buffer_size(const struct driver_dma_buffer *b){return b?sizeof(b->data):0;}
typedef struct {
    struct driver_dma_buffer *bulk_buffer;
    uint32_t contexts[33][16];
    spinlock_t lock;
    uintptr_t doorbell;
    xhci_ep_t *eps;
    uint64_t dcbaa[33];
    uint8_t fatal, max_ports, max_slots;
    xhci_slot_path_t slot_path[33];
} xhci_controller_t;
typedef struct{void*priv;}usb_hcd_t;
static unsigned xfer_busy,waits,bells;static uint32_t submitted_control;
static uint32_t*xhci_input_context(xhci_controller_t*x,unsigned n){assert(n<33);return x->contexts[n];}
static uintptr_t va_to_pa(const void*p){return (uintptr_t)p;}
static uint64_t spin_lock_irqsave(spinlock_t*l){assert(!l->held);l->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t*l,uint64_t f){(void)f;assert(l->held);l->held=0;}
#define XHCI_XFER_HOLD_BUDGET_MS 500U
#define XHCI_BULK_HOLD_BUDGET_MS 5000U
#define kwarn(...) ((void)0)
static int xhci_xfer_acquire(xhci_controller_t*x,unsigned budget){(void)x;assert(budget==XHCI_XFER_HOLD_BUDGET_MS||budget==XHCI_BULK_HOLD_BUDGET_MS);assert(!xfer_busy);xfer_busy=1;return 0;}
static void xhci_xfer_release(xhci_controller_t*x){(void)x;assert(xfer_busy);xfer_busy=0;}
static void arch_dma_sync_for_device(void*p,size_t n){(void)p;(void)n;}
static void arch_dma_sync_for_cpu(void*p,size_t n){(void)p;(void)n;}
static void wmb(void){}
static void xhci_write32(uintptr_t b,uint32_t o,uint32_t v){(void)b;assert(o==4&&v==3);bells++;}
static uint32_t xhci_read32(uintptr_t b,uint32_t o){(void)b;(void)o;return 0;}
static uint64_t xhci_ring_enqueue(xhci_ring_t*r,uint64_t p,uint32_t n,uint32_t c,int chain){
 (void)chain;
 assert(((c>>10)&63)==1 && !(c&(1U<<16))); /* Normal bit 16 is reserved. */
 unsigned i=r->enqueue++;r->trbs[i]=(xhci_trb_t){p,n,c};submitted_control=c;
 return (uintptr_t)&r->trbs[i];
}
static int xhci_wait_event(xhci_controller_t*x,uint8_t t,uint64_t p,
                           xhci_trb_t*out,const xhci_transfer_trbs_t*control){
 assert(control&&!control->bulk_first&&xfer_busy&&!x->lock.held&&t==32&&p);out->parameter=p;out->status=1U<<24;waits++;return 0;
}
'''

MAIN = r'''
int main(void){
 static const uint8_t expected[4][2]={{4,4},{1,5},{2,6},{3,7}};
 static struct driver_dma_buffer buffer;xhci_controller_t x={.bulk_buffer=&buffer};xhci_ring_t ring={.enqueue=17,.cycle=0};
 for(unsigned type=0;type<4;type++)for(unsigned in=0;in<2;in++){
  uint8_t value=xhci_ep_type(type,in?0x81:1);assert(value==expected[type][in]);
  memset(x.contexts,0,sizeof(x.contexts));
  xhci_fill_ep_context(&x,3,&ring,value,1024,0);
  uint32_t*ep=x.contexts[4];assert(((ep[1]>>3)&7)==value);
  assert(((ep[1]>>1)&3)==(type==1?0U:3U));
  assert((ep[1]>>16)==1024);
  assert((((uint64_t)ep[3]<<32)|ep[2])==(uintptr_t)&ring.trbs[17]);
  assert((ep[4]&0xffff)==(type==0?8U:1024U));
  assert((ep[4]>>16)==((type==1||type==3)?1024U:0U));
 }
 assert(!xhci_ep_type(255,0x81));
 uint32_t slot[4]={0x005abcdeU|(1U<<27)};
 xhci_slot_add_context(slot,4);assert((slot[0]>>27)==4);
 xhci_slot_add_context(slot,3);assert((slot[0]>>27)==4);
 xhci_slot_add_context(slot,7);assert((slot[0]>>27)==7);
 assert((slot[0]&0x07ffffffU)==0x005abcdeU);
 xhci_ep_t bulk={.slot=1,.dci=3};x.eps=&bulk;x.max_slots=32;x.dcbaa[1]=1;
 usb_device_t dev={.slot=1};usb_endpoint_t in={.addr=0x81,.max_packet=512};uint8_t data[64]={0};
 usb_urb_t urb={.ep=&in,.dev=&dev,.buf=data,.len=8};usb_hcd_t hcd={&x};
 /* READ CAPACITY(10) data and a CSW both use a bulk IN Normal TRB. */
 assert(!xhci_op_submit_bulk(&hcd,&urb));urb.len=13;
 assert(!xhci_op_submit_bulk(&hcd,&urb));
 assert(waits==2&&bells==2&&!xfer_busy&&(submitted_control&XHCI_TRB_IOC));
 /* HID's asynchronous path also leaves the reserved direction bit clear. */
 assert(!xhci_op_submit_interrupt(&hcd,&urb)&&bulk.pending==&urb);
 assert(bells==3&&waits==2);
 x.dcbaa[1]=0;bulk.pending=NULL;memset(data,0x6b,sizeof(data));
 assert(xhci_op_submit_interrupt(&hcd,&urb)==-ESHUTDOWN&&bells==3&&data[0]==0x6b);
 x.dcbaa[1]=1;

 /* Lenovo's high-speed disk is on port 3 of a high-speed hub at root 2.
  * A full-speed device on that port instead needs the hub's TT. */
 x.max_ports=7;
 x.max_slots=32;
 xhci_slot_path_t path;
 assert(!xhci_slot_path(&x,2,USB_SPEED_HIGH,0,&x.slot_path[4]));
 assert(!xhci_slot_path(&x,3,USB_SPEED_HIGH,4,&path));
 xhci_fill_slot_context(&x,&path);
 assert(x.contexts[1][0]==(1U<<27 | 3U<<20 | 3U));
 assert(x.contexts[1][1]==(2U<<16) && x.contexts[1][2]==0);
 assert(!xhci_slot_path(&x,3,USB_SPEED_FULL,4,&x.slot_path[5]));
 assert(!xhci_slot_path(&x,17,USB_SPEED_LOW,5,&path));
 xhci_fill_slot_context(&x,&path);
 assert(path.depth==2 && path.route==0xf3);
 assert(x.contexts[1][1]==(2U<<16) && x.contexts[1][2]==(3U<<8 | 4U));
 assert(xhci_slot_path(&x,8,USB_SPEED_HIGH,0,&path)==-EINVAL);
 assert(xhci_slot_path(&x,1,USB_SPEED_LOW,6,&path)==-EINVAL);
 return 0;
}
'''

CONTROL_MODEL = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

typedef struct {uint64_t parameter; uint32_t status, control;} xhci_trb_t;
typedef struct {uint64_t setup, data, bulk_first;} xhci_transfer_trbs_t;
typedef struct {
    _Alignas(16) xhci_trb_t trbs[XHCI_RING_TRBS];
    uint16_t enqueue;
    uint8_t cycle;
} xhci_ring_t;
typedef struct {
    uint8_t request_type, request;
    uint16_t value, index, length;
} usb_setup_packet_t;
typedef struct {
    int lock;
    uintptr_t doorbell;
    xhci_ring_t ep0_ring[2];
} xhci_controller_t;

static uint64_t spin_lock_irqsave(int *lock) { assert(!*lock); *lock = 1; return 0; }
static void spin_unlock_irqrestore(int *lock, uint64_t flags) {
    assert(*lock && flags == 0);
    *lock = 0;
}
static uintptr_t va_to_pa(const void *p) { return (uintptr_t)p; }
static void arch_dma_sync_for_device(void *p, size_t n) { (void)p; (void)n; }
static void wmb(void) {}
static void xhci_write32(uintptr_t base, uint32_t offset, uint32_t value) {
    assert(base == 0 && offset == 4 && value == 1);
}
static uint32_t xhci_read32(uintptr_t base, uint32_t offset) {
    assert(base == 0 && offset == 4);
    return 0;
}
'''

CONTROL_MAIN = r'''
int main(void) {
    xhci_controller_t xhci = {0};
    xhci_ring_t *ring = &xhci.ep0_ring[1];
    usb_setup_packet_t setup = { .request_type = 0x80, .request = 6,
                                 .value = 0x100, .length = 8 };
    uint8_t data[8];
    xhci_transfer_trbs_t control = {0};
    /* Put Setup immediately before the Link, so Data and Status use cycle 0. */
    ring->enqueue = XHCI_RING_TRBS - 2;
    ring->cycle = 1;
    uint64_t status = xhci_control_locked(&xhci, 1, &setup, data, &control);
    xhci_trb_t *first = &ring->trbs[XHCI_RING_TRBS - 2];
    assert(control.setup == (uintptr_t)first);
    assert(control.data == (uintptr_t)&ring->trbs[0]);
    assert(status == (uintptr_t)&ring->trbs[1]);
    assert(first->control == (XHCI_TRB_TYPE(2) | XHCI_TRB_CYCLE |
                              XHCI_TRB_IDT | XHCI_TRB_TRT_IN));
    assert(ring->trbs[XHCI_RING_TRBS - 1].control ==
           (XHCI_TRB_TYPE(6) | XHCI_TRB_CYCLE | (1U << 1)));
    assert(ring->trbs[0].control ==
           (XHCI_TRB_TYPE(3) | XHCI_TRB_DIR_IN | XHCI_TRB_ISP));
    assert(ring->trbs[1].control == (XHCI_TRB_TYPE(4) | XHCI_TRB_IOC));

    xhci_trb_t event = {control.setup, 5U << 24, XHCI_TRB_TYPE(32)};
    assert(xhci_event_matches(&event, 32, status, &control));
    event.parameter = control.data;
    assert(xhci_event_matches(&event, 32, status, &control));
    event.status = XHCI_CC_SHORT_PACKET << 24;
    assert(!xhci_event_matches(&event, 32, status, &control));
    event.parameter = status;
    event.status = XHCI_CC_SUCCESS << 24;
    assert(xhci_event_matches(&event, 32, status, &control));
    assert(!xhci_event_matches(&event, 33, status, &control));
    return 0;
}
'''


class XhciEndpointTypes(unittest.TestCase):
    def test_production_endpoint_and_transfer_encoding(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        bodies = "\n".join(function(source, name) for name in (
            "xhci_ep_type", "xhci_slot_add_context", "xhci_slot_path",
            "xhci_fill_slot_context", "xhci_fill_ep_context", "xhci_find_ep",
            "xhci_op_submit_interrupt", "xhci_op_submit_bulk"))
        with tempfile.TemporaryDirectory(prefix="a20-xhci-types-") as tmp:
            work = Path(tmp)
            path = work / "model.c"
            path.write_text(MODEL + bodies + MAIN)
            exe = work / "model"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(path), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)

    def test_control_stages_and_early_completion(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        defines = re.findall(r"^#define (?:XHCI_(?:RING_TRBS|TRB_\w+|CC_\w+)|USB_DIR_IN)\b.*$",
                             source, re.M)
        bodies = "\n".join(function(source, name) for name in (
            "xhci_ring_enqueue", "xhci_event_matches", "xhci_control_locked"))
        with tempfile.TemporaryDirectory(prefix="a20-xhci-control-") as tmp:
            path = Path(tmp) / "model.c"
            exe = Path(tmp) / "model"
            path.write_text("\n".join(defines) + CONTROL_MODEL + bodies + CONTROL_MAIN)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(path), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)

    def test_input_context_layout_for_both_context_sizes(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        body = function(source, "xhci_input_context")
        model = '''
#include <assert.h>
#include <stddef.h>
typedef struct { unsigned char input_context[2112]; unsigned char context_size; } xhci_controller_t;
''' + body + '''
int main(void) {
    xhci_controller_t c = {0};
    c.context_size = 64;
    assert(xhci_input_context(&c, 0) == c.input_context);
    assert(xhci_input_context(&c, 1) == c.input_context + 64);
    assert(xhci_input_context(&c, 2) == c.input_context + 128);
    assert(xhci_input_context(&c, 32) == c.input_context + 2048);
    c.context_size = 32;
    assert(xhci_input_context(&c, 1) == c.input_context + 32);
    assert(xhci_input_context(&c, 2) == c.input_context + 64);
    return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix="a20-xhci-ctx-") as tmp:
            path = Path(tmp) / "model.c"
            exe = Path(tmp) / "model"
            path.write_text(model)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(path), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)


if __name__ == "__main__":
    unittest.main()

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
#define XHCI_TRB_CYCLE 1U
#define XHCI_TRB_TYPE(n) ((n)<<10)
#define XHCI_TRB_NORMAL 1U
#define XHCI_TRB_TRANSFER_EVENT 32U
#define XHCI_TRB_ISP (1U<<2)
#define XHCI_TRB_IOC (1U<<5)
typedef struct{uint64_t parameter;uint32_t status,control;}xhci_trb_t;
typedef struct{xhci_trb_t trbs[64];uint16_t enqueue;uint8_t cycle;}xhci_ring_t;
typedef struct{uint8_t addr;}usb_endpoint_t;
typedef struct{uint8_t slot;}usb_device_t;
typedef struct usb_urb{usb_endpoint_t*ep;usb_device_t*dev;void*buf;size_t len;}usb_urb_t;
typedef struct xhci_ep{struct xhci_ep*next;uint8_t slot,dci;xhci_ring_t ring;usb_urb_t*pending;}xhci_ep_t;
typedef struct{int held;}spinlock_t;
typedef struct{uint32_t contexts[33][16];spinlock_t lock;uintptr_t doorbell;xhci_ep_t*eps;}xhci_controller_t;
typedef struct{void*priv;}usb_hcd_t;
static unsigned xfer_busy,waits,bells;static uint32_t submitted_control;
static uint32_t*xhci_input_context(xhci_controller_t*x,unsigned n){assert(n<33);return x->contexts[n];}
static uintptr_t va_to_pa(const void*p){return (uintptr_t)p;}
static uint64_t spin_lock_irqsave(spinlock_t*l){assert(!l->held);l->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t*l,uint64_t f){(void)f;assert(l->held);l->held=0;}
static int xhci_xfer_acquire(xhci_controller_t*x){(void)x;assert(!xfer_busy);xfer_busy=1;return 0;}
static void xhci_xfer_release(xhci_controller_t*x){(void)x;assert(xfer_busy);xfer_busy=0;}
static void arch_dma_sync_for_device(void*p,size_t n){(void)p;(void)n;}
static void arch_dma_sync_for_cpu(void*p,size_t n){(void)p;(void)n;}
static void wmb(void){}
static void xhci_write32(uintptr_t b,uint32_t o,uint32_t v){(void)b;assert(o==4&&v==3);bells++;}
static uint64_t xhci_ring_enqueue(xhci_ring_t*r,uint64_t p,uint32_t n,uint32_t c){
 assert(((c>>10)&63)==1 && !(c&(1U<<16))); /* Normal bit 16 is reserved. */
 unsigned i=r->enqueue++;r->trbs[i]=(xhci_trb_t){p,n,c};submitted_control=c;
 return (uintptr_t)&r->trbs[i];
}
static int xhci_wait_event(xhci_controller_t*x,uint8_t t,uint64_t p,xhci_trb_t*out){
 (void)out;assert(xfer_busy&&!x->lock.held&&t==32&&p);waits++;return 0;
}
'''

MAIN = r'''
int main(void){
 static const uint8_t expected[4][2]={{4,4},{1,5},{2,6},{3,7}};
 xhci_controller_t x={0};xhci_ring_t ring={.enqueue=17,.cycle=0};
 for(unsigned type=0;type<4;type++)for(unsigned in=0;in<2;in++){
  uint8_t value=xhci_ep_type(type,in?0x81:1);assert(value==expected[type][in]);
  memset(x.contexts,0,sizeof(x.contexts));
  xhci_fill_ep_context(&x,3,&ring,value,1024,0);
  uint32_t*ep=x.contexts[4];assert(((ep[1]>>3)&7)==value);
  assert((ep[1]>>16)==1024);
  assert((((uint64_t)ep[3]<<32)|ep[2])==(uintptr_t)&ring.trbs[17]);
  assert((ep[4]>>16)==((type==1||type==3)?1024U:0U));
 }
 assert(!xhci_ep_type(255,0x81));
 uint32_t slot[4]={0x005abcdeU|(1U<<27)};
 xhci_slot_add_context(slot,4);assert((slot[0]>>27)==4);
 xhci_slot_add_context(slot,3);assert((slot[0]>>27)==4);
 xhci_slot_add_context(slot,7);assert((slot[0]>>27)==7);
 assert((slot[0]&0x07ffffffU)==0x005abcdeU);
 xhci_ep_t bulk={.slot=1,.dci=3};x.eps=&bulk;
 usb_device_t dev={.slot=1};usb_endpoint_t in={.addr=0x81};uint8_t data[64]={0};
 usb_urb_t urb={.ep=&in,.dev=&dev,.buf=data,.len=8};usb_hcd_t hcd={&x};
 /* READ CAPACITY(10) data and a CSW both use a bulk IN Normal TRB. */
 assert(!xhci_op_submit_bulk(&hcd,&urb));urb.len=13;
 assert(!xhci_op_submit_bulk(&hcd,&urb));
 assert(waits==2&&bells==2&&!xfer_busy&&(submitted_control&XHCI_TRB_IOC));
 /* HID's asynchronous path also leaves the reserved direction bit clear. */
 assert(!xhci_op_submit_interrupt(&hcd,&urb)&&bulk.pending==&urb);
 assert(bells==3&&waits==2);
 return 0;
}
'''


class XhciEndpointTypes(unittest.TestCase):
    def test_production_endpoint_and_transfer_encoding(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        bodies = "\n".join(function(source, name) for name in (
            "xhci_ep_type", "xhci_slot_add_context", "xhci_fill_ep_context", "xhci_find_ep",
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


if __name__ == "__main__":
    unittest.main()

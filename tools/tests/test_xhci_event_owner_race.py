"""Replay callback unlock -> new sync owner -> completion, using production drain."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
MODEL=r'''
#include <stdint.h>
#include <assert.h>
#define XHCI_IRQ_BATCH 8
typedef struct {unsigned type;} xhci_trb_t;
typedef struct {unsigned kind;} usb_urb_t;
typedef struct {int xfer_busy;} xhci_controller_t;
static xhci_trb_t events[8];static unsigned head,tail,callbacks;
static unsigned inject_type;static int inject_owner;
static usb_urb_t keyboard;
static int xhci_next_event(xhci_controller_t*x,xhci_trb_t*e){
 (void)x;if(tail==head)return 0;*e=events[tail++];return 1;
}
static unsigned xhci_collect_interrupt(xhci_controller_t*x,const xhci_trb_t*e,usb_urb_t**out){
 (void)x;if(e->type!=1)return 0;*out=&keyboard;return 1;
}
static void xhci_run_completions(usb_urb_t**r,unsigned n,xhci_controller_t*x,uint64_t*f){
 (void)r;(void)f;assert(n>0);callbacks+=n;
 /* A second CPU claims xfer_busy while the IRQ callback runs unlocked. */
 if(inject_type){x->xfer_busy=inject_owner;events[head++]=(xhci_trb_t){inject_type};inject_type=0;}
}
'''
MAIN=r'''
int main(void){
 for(unsigned type=2;type<=3;type++){
  xhci_controller_t x={0};uint64_t flags=0;head=1;tail=callbacks=0;
  events[0]=(xhci_trb_t){1};inject_type=type;inject_owner=1;
  assert(xhci_drain_events(&x,&flags)==1);
  assert(callbacks==1 && tail==1 && head==2); /* Sync waiter must own its event. */
  xhci_trb_t completion;assert(xhci_next_event(&x,&completion));assert(completion.type==type);
 }
 xhci_controller_t x={0};uint64_t flags=0;head=1;tail=callbacks=0;events[0]=(xhci_trb_t){1};
 inject_type=1;inject_owner=0;assert(xhci_drain_events(&x,&flags)==1);assert(callbacks==2&&tail==head);
 x.xfer_busy=1;head=1;tail=0;events[0]=(xhci_trb_t){2};
 assert(!xhci_drain_events(&x,&flags)&&tail==0);
 return 0;
}
'''

class XhciEventOwnerRace(unittest.TestCase):
    def test_callback_handoff_keeps_sync_completion(self):
        body=function((ROOT/'kernel/drivers/usb/host/xhci.c').read_text(),'xhci_drain_events')
        with tempfile.TemporaryDirectory(prefix='a20-xhci-owner-') as tmp:
            p=Path(tmp)
            def run(code,name):
                (p/(name+'.c')).write_text(MODEL+code+MAIN)
                result=subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/(name+'.c')),'-o',str(p/name)],capture_output=True,text=True)
                self.assertEqual(result.returncode,0,result.stderr)
                return subprocess.run([str(p/name)],capture_output=True,text=True)
            current=run(body,'current')
            self.assertEqual(current.returncode,0,current.stderr)
            old=body.replace('        if (xhci->xfer_busy)\n            return handled;\n','')
            if old!=body:self.assertNotEqual(run(old,'negative').returncode,0)


class XhciTimeoutQuiesce(unittest.TestCase):
    def test_terminal_timeout_stops_dma_before_return(self):
        body=function((ROOT/'kernel/drivers/usb/host/xhci.c').read_text(),'xhci_quiesce_failed')
        code=r'''
#include <stdint.h>
#include <assert.h>
#define XHCI_INTERRUPTER_OFFSET 0x20
#define XHCI_USBCMD 0
#define XHCI_USBSTS 4
#define XHCI_CMD_RUN 1
#define XHCI_CMD_INTE 4
#define XHCI_STS_HALTED 1
typedef struct{int lock,fatal,running;uintptr_t op,runtime;struct{void*hcd_dev;}hcd;}xhci_controller_t;
static unsigned cmd=5,iman=3,pci=7;static int locked;
static uint64_t spin_lock_irqsave(int*l){(void)l;assert(!locked);locked=1;return 0;}
static void spin_unlock_irqrestore(int*l,uint64_t f){(void)l;(void)f;assert(locked);locked=0;}
static void xhci_write32(uintptr_t b,unsigned o,unsigned v){assert(locked);if(b==0x100){assert(o==0);cmd=v;}else{assert(b==0x220&&o==0);iman=v;}}
static unsigned xhci_read32(uintptr_t b,unsigned o){assert(locked&&b==0x100&&o==0);return cmd;}
static uint16_t pci_cfg_read16(void*d,unsigned o){assert(locked&&d&&o==4);return pci;}
static void pci_cfg_write16(void*d,unsigned o,uint16_t v){assert(locked&&d&&o==4);pci=v;}
static int xhci_wait32(uintptr_t b,unsigned o,unsigned mask,unsigned expected){assert(!locked&&b==0x100&&o==4&&mask==1&&expected==1);assert(cmd==0&&iman==0&&!(pci&4));return 0;}
'''
        code+=body+r'''
int main(void){xhci_controller_t x={.running=1,.op=0x100,.runtime=0x200,.hcd={&x}};
assert(!xhci_quiesce_failed(&x));assert(x.fatal&&!x.running&&pci==3);return 0;}
'''
        with tempfile.TemporaryDirectory(prefix='a20-xhci-halt-') as tmp:
            p=Path(tmp);(p/'test.c').write_text(code)
            subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',str(p/'test.c'),'-o',str(p/'test')],check=True)
            subprocess.run([str(p/'test')],check=True)

if __name__=='__main__':unittest.main()

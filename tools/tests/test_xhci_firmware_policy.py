"""Execute production xHCI ownership, Bay Trail routing and DMA policy."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    found = re.search(r"^static [^\n]*\b" + name +
                      r"\([^{}]*\)\s*\{.*?^\}", source, re.M | re.S)
    if not found:
        raise AssertionError(f"missing production function {name}")
    return found.group()


MODEL = r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#define DMA_MASK_64BIT UINT64_MAX
#define DMA_MASK_32BIT UINT32_MAX
#define XHCI_FIRMWARE_TIMEOUT_MS 1000U
typedef struct {uint32_t id;} device_t;
static uint32_t mmio[1024],cfg[64];
static unsigned delays,release_at,mmio_writes,cfg_reads,cfg_writes,write_reg[4];
static int mask_result,locked;
static uint64_t mask_seen;
static uint32_t xhci_read32(uintptr_t b,uint32_t o) {
 assert(b==(uintptr_t)mmio && o<sizeof(mmio)); return mmio[o/4];
}
static void xhci_write32(uintptr_t b,uint32_t o,uint32_t v) {
 assert(b==(uintptr_t)mmio && o<sizeof(mmio)); mmio_writes++;
 if(o==0x100) mmio[o/4]=(mmio[o/4]&(1U<<16))|(v&~(1U<<16));
 else mmio[o/4]=v;
}
static void mdelay(unsigned n) {
 assert(n==1); delays++;
 if(delays==release_at) mmio[0x100/4]&=~(1U<<16);
}
static uint32_t pci_device_id(const device_t*d) {return d->id;}
static uint32_t pci_cfg_read32(const device_t*d,uint32_t r) {
 (void)d; assert(r<sizeof(cfg)); cfg_reads++; return cfg[r/4];
}
static void pci_cfg_write32(const device_t*d,uint32_t r,uint32_t v) {
 (void)d; assert(r==0xd0||r==0xd8); assert(cfg_writes<4);
 write_reg[cfg_writes++]=r; if(!locked)cfg[r/4]=v;
}
static int dma_set_mask(device_t*d,uint64_t m) {(void)d;mask_seen=m;return mask_result;}
'''
MAIN = r'''
int main(int argc,char**argv) {
 assert(argc==2); device_t d={0x80860f35U}; unsigned test=atoi(argv[1]);
 switch(test) {
 case 0: assert(!xhci_take_ownership((uintptr_t)mmio,sizeof(mmio)) && !mmio_writes);break;
 case 1:
  mmio[0x10/4]=0x40U<<16; mmio[0x100/4]=1|(1U<<16);
  mmio[0x104/4]=UINT32_MAX; release_at=7;
  assert(!xhci_take_ownership((uintptr_t)mmio,sizeof(mmio)));
  assert(delays==7 && mmio_writes==2 && (mmio[0x100/4]&(1U<<24)));
  assert(mmio[0x104/4]==((7U<<1)|(255U<<5)|(7U<<17)|(7U<<29)));break;
 case 2:
  mmio[0x10/4]=0x40U<<16;mmio[0x100/4]=1|(1U<<16);mmio[0x104/4]=0xabc;
  assert(xhci_take_ownership((uintptr_t)mmio,sizeof(mmio))==-ETIMEDOUT);
  assert(delays==1000 && mmio_writes==1 && mmio[0x104/4]==0xabc);break;
 case 3:
  mmio[0x10/4]=0x3ffU<<16;mmio[0xffc/4]=1;
  assert(xhci_take_ownership((uintptr_t)mmio,sizeof(mmio))==-EINVAL);
  assert(!mmio_writes);break;
 case 4:
  mmio[0x10/4]=1U<<16;
  assert(xhci_take_ownership((uintptr_t)mmio,sizeof(mmio))==-EINVAL);break;
 case 5: /* Relative next pointers are walked before the optional legacy cap. */
  mmio[0x10/4]=0x3fU<<16;mmio[0xfc/4]=2|(1U<<8);mmio[0x100/4]=1;
  assert(!xhci_take_ownership((uintptr_t)mmio,sizeof(mmio)) && mmio_writes==2);break;
 case 6:
  mmio[0x10/4]=0x40U<<16;
  for(unsigned i=0;i<64;i++)mmio[0x100/4+i]=2|(1U<<8);
  assert(xhci_take_ownership((uintptr_t)mmio,sizeof(mmio))==-EINVAL);break;
 case 7:
  d.id=0x80861e31;assert(!xhci_baytrail_route_ports(&d));assert(!cfg_reads&&!cfg_writes);break;
 case 8:
  cfg[0xdc/4]=1;cfg[0xd4/4]=15;cfg[0xd8/4]=0x80000000;cfg[0xd0/4]=0x40000002;
  assert(!xhci_baytrail_route_ports(&d));
  assert(cfg_writes==2&&write_reg[0]==0xd8&&write_reg[1]==0xd0);
  assert(cfg[0xd8/4]==0x80000001&&cfg[0xd0/4]==0x4000000f);
  assert(!xhci_baytrail_route_ports(&d)&&cfg_writes==2);break;
 case 9:
  cfg[0xdc/4]=UINT32_MAX;cfg[0xd4/4]=15;
  assert(xhci_baytrail_route_ports(&d)==-EIO&&!cfg_writes);break;
 case 10:
  cfg[0xdc/4]=1;cfg[0xd4/4]=0x1000;
  assert(xhci_baytrail_route_ports(&d)==-EIO&&!cfg_writes);break;
 case 11:
  cfg[0xdc/4]=1;cfg[0xd4/4]=15;locked=1;
  assert(xhci_baytrail_route_ports(&d)==-EIO&&cfg_writes==2);break;
 case 12:
  assert(!xhci_configure_dma_mask(&d,0)&&mask_seen==UINT32_MAX);
  assert(!xhci_configure_dma_mask(&d,1)&&mask_seen==UINT64_MAX);
  mask_result=-EOPNOTSUPP;assert(xhci_configure_dma_mask(&d,0)==mask_result);break;
 default:abort();
 }
 return 0;
}
'''


class XhciFirmwarePolicy(unittest.TestCase):
    def test_production_register_policy(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        bodies = "\n".join(function(source, n) for n in (
            "xhci_take_ownership", "xhci_configure_dma_mask",
            "xhci_baytrail_route_ports"))
        with tempfile.TemporaryDirectory(prefix="a20-xhci-fw-") as tmp:
            path = Path(tmp) / "model.c"
            path.write_text(MODEL + bodies + MAIN)
            exe = Path(tmp) / "model"
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(path), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            for case in range(13):
                with self.subTest(case=case):
                    subprocess.run([str(exe), str(case)], check=True,
                                   capture_output=True, text=True)

    def test_integration_order(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        start = function(source, "xhci_op_start")
        self.assertLess(start.index("xhci_take_ownership"), start.index("XHCI_CMD_RESET"))
        probe = function(source, "xhci_probe")
        self.assertLess(probe.index("xhci_configure_dma_mask"),
                        probe.index("xhci_alloc_controller"))
        self.assertLess(probe.index("usb_core_register_hcd"),
                        probe.index("xhci_baytrail_route_ports"))
        core = (ROOT / "kernel/drivers/usb/core/usb_core.c").read_text()
        register = core[core.index("int usb_core_register_hcd("):
                        core.index("void usb_core_unregister_hcd(")]
        self.assertNotIn("usb_enumerate", register)
        self.assertNotIn("usb_core_scan", register)


if __name__ == "__main__":
    unittest.main()

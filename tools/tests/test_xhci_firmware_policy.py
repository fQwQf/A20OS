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

PORT_MODEL = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#define XHCI_WAIT_LOOPS 4U
#define kinfo(...) ((void)0)
typedef struct {
    uintptr_t op;
    unsigned fatal, max_ports, port_power_control;
} xhci_controller_t;
typedef struct { void *priv; } usb_hcd_t;
static uint32_t portsc;
static unsigned writes;
static uint32_t xhci_read32(uintptr_t base, uint32_t offset) {
    assert(base == 0 && offset == 0x400);
    return portsc;
}
static void xhci_write32(uintptr_t base, uint32_t offset, uint32_t value) {
    assert(base == 0 && offset == 0x400);
    writes++;
    /* xHCI 1.2 table 5-27. Model write-one-to-clear separately from
     * persistent fields, and complete a USB 2 reset immediately. */
    const uint32_t persistent = (0xfU << 5) | (1U << 9) |
                                (3U << 14) | (7U << 25);
    const uint32_t clear = (1U << 1) | (0x7fU << 17);
    portsc = (portsc & ~persistent) | (value & persistent);
    portsc &= ~(value & clear);
    if (value & (1U << 4))
        portsc |= (1U << 21) | (1U << 1);
}
static void mdelay(unsigned ms) { assert(ms == 20); }
static void arch_cpu_relax(void) {}
'''

PORT_MAIN = r'''
int main(void) {
    xhci_controller_t x = {.max_ports = 1, .port_power_control = 1};
    usb_hcd_t hcd = {&x};
    const uint32_t connected = 1U | (1U << 10) | (1U << 25);
    uint8_t speed = 0;
    portsc = connected | (1U << 1) | (1U << 17);
    xhci_power_ports(&x);
    assert(writes == 1 && portsc == (connected | (1U << 1) |
                                    (1U << 9) | (1U << 17)));
    assert(!xhci_op_reset_port(&hcd, 1, &speed));
    assert(writes == 3 && speed == 1);
    assert(portsc == (connected | (1U << 9) | (1U << 1)));
    portsc = connected;
    assert(!xhci_op_reset_port(&hcd, 1, &speed));
    assert(writes == 6 && portsc == (connected | (1U << 9) | (1U << 1)));
    assert(xhci_op_reset_port(&hcd, 0, &speed) == -ENODEV);
    assert(xhci_op_reset_port(&hcd, 2, &speed) == -ENODEV);
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

    def test_interrupt_and_port_power_policy(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        # xHCI 1.2 5.5.2.1: IMAN bit 0 is IP (RW1C), bit 1 is IE.  Reversing
        # them makes the handler test IE instead of IP and leaves a real
        # controller's level-triggered INTx line asserted.
        self.assertIn("#define XHCI_INTR0_IP                 (1U << 0)", source)
        self.assertIn("#define XHCI_INTR0_IE                 (1U << 1)", source)
        start = function(source, "xhci_op_start")
        self.assertIn("xhci_power_ports(xhci);", start)
        # Linux's xhci_mem_init writes CONFIG, then DCBAAP, then CRCR.  The
        # old code wrote CRCR before DCBAAP; keep the order asserted here so a
        # future edit cannot reintroduce the controller-validates-CRCR path.
        self.assertLess(start.index("XHCI_CONFIG"), start.index("XHCI_DCBAAP"))
        self.assertLess(start.index("XHCI_DCBAAP"),
                        start.index("xhci_program_crcr"))
        crcr = function(source, "xhci_program_crcr")
        self.assertIn("XHCI_CRCR", crcr)
        self.assertIn("current & 0x3fU", crcr)
        command = function(source, "xhci_command_locked")
        self.assertIn("xhci_read32(xhci->doorbell, 0)", command)
        self.assertIn("#define XHCI_CTRL_ALIGN PAGE_SIZE", source)
        self.assertIn("XHCI_CTRL_ALIGN - 1UL", source)
        power = function(source, "xhci_power_ports")
        self.assertIn("XHCI_PORT_POWER", power)
        reset = function(source, "xhci_op_reset_port")
        self.assertIn("XHCI_PORT_POWER", reset)

    def test_root_port_reset_preserves_enabled_state(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        definitions = re.findall(r"^#define XHCI_PORT.*$",
                                 source.replace("\\\n", " "), re.M)
        bodies = "\n".join(function(source, name) for name in (
            "xhci_op_reset_port", "xhci_power_ports"))
        with tempfile.TemporaryDirectory(prefix="a20-xhci-port-") as tmp:
            path = Path(tmp) / "model.c"
            exe = Path(tmp) / "model"
            path.write_text(PORT_MODEL + "\n".join(definitions) + "\n" +
                            bodies + PORT_MAIN)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined", str(path), "-o", str(exe)],
                           check=True, capture_output=True, text=True)
            subprocess.run([str(exe)], check=True, capture_output=True, text=True)

    def test_alloc_slot_retains_known_context_on_address_failure(self):
        source = (ROOT / "kernel/drivers/usb/host/xhci.c").read_text()
        body = function(source, "xhci_op_alloc_slot")
        model = (ROOT / "tools/tests/xhci_slot_lifetime_host.c").read_text()
        main = r'''
int main(int argc,char **argv) {
 assert(argc==2);which=atoi(argv[1]);
 xhci_controller_t x={.max_slots=2,.context_size=32};usb_hcd_t h={&x};
 usb_slot_t token={.hcd=99,.address=99};active_token=&token;
 if(which==8)x.fatal=1;
 assert(xhci_op_alloc_slot(&h,1,3,0,1,NULL)==-EINVAL&&!calls);
 int ret=xhci_op_alloc_slot(&h,1,3,0,1,&token);
 if(which==0)assert(!ret&&token.hcd==1&&token.address==7&&delays==1&&x.slot_path[1].route==17);
 else if(which<=3)assert(ret==(which==3?-ETIMEDOUT:-EIO)&&token.hcd==1&&!token.address&&x.dcbaa[1]);
 else {
  int error=which==4?-EBUSY:which==5?-EINVAL:which==8?-ENODEV:-EIO;
  assert(ret==error&&!token.hcd&&!token.address&&!delays);
 }
 assert(!owned&&calls==(which<=3?2:which==6||which==7?1:0));
 assert(releases==(which==4||which==8?0:1));return 0;
}
'''
        with tempfile.TemporaryDirectory(prefix='a20-xhci-slot-') as tmp:
            path = Path(tmp)
            (path / 'test.c').write_text(model + body + main)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=address,undefined', '-fno-sanitize-recover=all',
                            str(path / 'test.c'), '-o', str(path / 'test')], check=True)
            for case in range(9):
                with self.subTest(case=case):
                    subprocess.run([str(path / 'test'), str(case)], check=True)

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
                        core.index("int usb_core_unregister_hcd(")]
        self.assertNotIn("usb_enumerate", register)
        self.assertNotIn("usb_core_scan", register)


if __name__ == "__main__":
    unittest.main()

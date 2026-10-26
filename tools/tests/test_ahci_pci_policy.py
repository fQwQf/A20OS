"""Compile AHCI's actual PCI/ownership helpers against register models.

These tests cover firmware handoff, PCS preservation and DMA addressing, not
physical SATA link integrity.  The helper bodies are taken verbatim from the
production translation unit so the model cannot silently replace the policy.
"""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/drivers/block/ahci.c"


def function(source, name):
    found = re.search(r"^static [^\n]*\b" + name +
                      r"\([^{}]*\)\s*\{.*?^\}", source, re.M | re.S)
    if not found:
        raise AssertionError(f"missing production function {name}")
    return found.group()


STUB = r'''
#include <stdint.h>
#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#define DMA_MASK_64BIT UINT64_MAX
#define DMA_MASK_32BIT UINT32_MAX
#define AHCI_PLATFORM_F_READ_ONLY 1
typedef struct { void *bus; uint32_t id, class_code; uint16_t pcs; } device_t;
static int pci_bus, platform_bus;
static unsigned cfg_reads, cfg_writes, delays, mmio_reads, mmio_writes;
static unsigned clear_bos_after, clear_bb_after, enabled, common_called;
static int mask_result, common_result;
static uint64_t dma_mask;
static uint32_t regs[16];
static volatile void *ahci_reg(uintptr_t b,uint32_t o) { return (void *)(b+o); }
static uint32_t readl(const volatile void *p) {
    uintptr_t offset=(uintptr_t)p-(uintptr_t)regs;
    assert(offset<sizeof(regs)); mmio_reads++;
    return *(const volatile uint32_t *)p;
}
static void writel(uint32_t v,volatile void *p) {
    assert((uintptr_t)p-(uintptr_t)regs==0x28); mmio_writes++;
    /* BIOS owner/busy are hardware controlled, OOC is W1C. */
    regs[0x28/4]=(regs[0x28/4]&0x11U)|(v&6U);
}
static void mdelay(unsigned ms) {
    assert(ms==1); delays++;
    if(delays==clear_bos_after) regs[0x28/4]&=~1U;
    if(delays==clear_bb_after) regs[0x28/4]&=~16U;
}
static uint32_t pci_device_id(const device_t*d) { return d->id; }
static uint32_t pci_class_code(const device_t*d) { return d->class_code; }
static uint16_t pci_cfg_read16(const device_t*d,uint32_t r) {
    assert(r==0x92); cfg_reads++; return d->pcs;
}
static void pci_cfg_write16(const device_t*d,uint32_t r,uint16_t v) {
    assert(r==0x92); cfg_writes++; ((device_t *)d)->pcs=v;
}
static int dma_set_mask(device_t*d,uint64_t m) {
    (void)d; dma_mask=m; return mask_result;
}
static int pci_enable_and_assign_bars(device_t*d) { (void)d; enabled++; return 0; }
static int pci_intx_irq(device_t*d) { (void)d; return -1; }
static int ahci_probe_common(device_t*d,int irq,uint32_t flags,uint32_t map) {
    (void)d; assert(irq==-1 && !flags && !map); common_called++; return common_result;
}
'''

HARNESS = r'''
int main(int argc,char **argv) {
    assert(argc==2); unsigned test=(unsigned)atoi(argv[1]);
    device_t d={.bus=&pci_bus,.id=0x80860f23,.class_code=0x010601};
    switch(test) {
    case 0: /* Both Bay Trail AHCI functions, unchanged status/other bits. */
        for(unsigned id=0x0f22;id<=0x0f23;id++) {
            d.id=0x80860000|id; d.pcs=0xa540;
            ahci_pci_enable_ports(&d,3); assert(d.pcs==0xa543);
        }
        assert(cfg_reads==2 && cfg_writes==2); break;
    case 1: /* No redundant writes, no unadvertised port enable. */
        d.pcs=0x8002; ahci_pci_enable_ports(&d,2);
        assert(cfg_reads==1 && !cfg_writes && d.pcs==0x8002); break;
    case 2: /* PCS status bits must never be copied from a bogus PI bitmap. */
        d.pcs=0x8000; ahci_pci_enable_ports(&d,0xffff0001);
        assert(d.pcs==0x8001 && cfg_writes==1); break;
    case 3: /* No quirk on newer Intel, another vendor or platform MMIO. */
        d.id=0x80865ae3; ahci_pci_enable_ports(&d,3);
        d.id=0x12340f23; ahci_pci_enable_ports(&d,3);
        d.id=0x80860f23; d.bus=&platform_bus; ahci_pci_enable_ports(&d,3);
        assert(!cfg_reads && !cfg_writes); break;
    case 4: /* Legacy ICH support and its larger six-port bitmap. */
        d.id=0x80862922; d.pcs=0x4000; ahci_pci_enable_ports(&d,0x3f);
        assert(d.pcs==0x403f); break;
    case 5: /* 32-bit and 64-bit masks, allocator failure propagated. */
        assert(!ahci_configure_dma_mask(&d,0) && dma_mask==UINT32_MAX);
        assert(!ahci_configure_dma_mask(&d,1U<<31) && dma_mask==UINT64_MAX);
        mask_result=-EOPNOTSUPP;
        assert(ahci_configure_dma_mask(&d,0)==-EOPNOTSUPP); break;
    case 6: /* Old AHCI revision must not touch undefined CAP2/BOHC. */
        regs[0x24/4]=1; regs[0x28/4]=17;
        assert(!ahci_take_ownership((uintptr_t)regs,0x00010100));
        assert(!mmio_reads && !mmio_writes); break;
    case 7: /* Non-BOH HBA does not acquire a nonexistent semaphore. */
        assert(!ahci_take_ownership((uintptr_t)regs,0x00010300));
        assert(mmio_reads==1 && !mmio_writes); break;
    case 8: /* Owner clears first, BIOS busy later; wait for both. */
        regs[0x24/4]=1; regs[0x28/4]=17;
        clear_bos_after=2; clear_bb_after=9;
        assert(!ahci_take_ownership((uintptr_t)regs,0x00010301));
        assert(delays==9 && mmio_writes==1 && regs[0x28/4]==2); break;
    case 9: /* Bounded refusal: no reset or forced clear of a stuck BIOS. */
        regs[0x24/4]=1; regs[0x28/4]=17;
        assert(ahci_take_ownership((uintptr_t)regs,0x00010200)==-ETIMEDOUT);
        assert(delays==AHCI_TIMEOUT_MS && mmio_writes==1 && regs[0x28/4]==19); break;
    case 10: /* Same device ID in IDE mode must not touch BAR/enable state. */
        d.class_code=0x01018f; assert(ahci_pci_probe(&d)==-ENODEV);
        assert(!enabled && !common_called);
        d.class_code=0x010601; common_result=-EOPNOTSUPP;
        assert(ahci_pci_probe(&d)==-EOPNOTSUPP);
        assert(enabled==1 && common_called==1); break;
    default: abort();
    }
    return 0;
}
'''


class AhciPciPolicy(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-ahci-pci-")
        cls.addClassCleanup(cls.tmp.cleanup)
        cls.source = SOURCE.read_text()
        defines = "\n".join(re.findall(r"^#define AHCI_.*$", cls.source, re.M))
        bodies = "\n".join(function(cls.source, name) for name in (
            "ahci_take_ownership", "ahci_configure_dma_mask",
            "ahci_pci_enable_ports", "ahci_pci_probe"))
        path = Path(cls.tmp.name) / "policy.c"
        path.write_text(STUB + "\n" + defines + "\n" + bodies + HARNESS)
        cls.exe = Path(cls.tmp.name) / "policy"
        subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-fsanitize=undefined", "-g", str(path), "-o", str(cls.exe)],
                       check=True, capture_output=True, text=True)

    def test_register_protocol(self):
        for case in range(11):
            with self.subTest(case=case):
                subprocess.run([str(self.exe), str(case)], check=True,
                               capture_output=True, text=True)

    def test_probe_integration(self):
        body = function(self.source, "ahci_probe_common")
        self.assertIn("pci_get_bar_resource(dev, 5)", body)
        self.assertIn("pci_map_bar(dev, 5)", body)
        self.assertLess(body.index("ahci_configure_dma_mask(dev, cap)"),
                        body.index("dma_alloc_coherent_aligned"))
        self.assertLess(body.index("ahci_take_ownership(port->host_regs, version)"),
                        body.index("ghc | AHCI_GHC_AE | AHCI_GHC_HR"))
        self.assertLess(body.index("ahci_pci_enable_ports(dev, pi)"),
                        body.index("ahci_wait_link(port)"))
        for device in ("0x0f22U", "0x0f23U"):
            self.assertIn(".device = " + device, self.source)
        module = (ROOT / "kernel/drvmod/examples/ahci.c").read_text()
        for device in ("0x0f22", "0x0f23"):
            self.assertIn("A20_DRIVER_MATCH(A20_DRIVER_BUS_PCI, 0x8086, " + device, module)


if __name__ == "__main__":
    unittest.main()

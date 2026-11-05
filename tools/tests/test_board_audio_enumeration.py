"""Real DT enumeration -> board audio metadata before any driver IRQ prepare.

No firmware IRQ API, domain decoder, device lookup or board helper is mocked.
Every MMIO operation traps, proving configure_metadata remains metadata-only.
"""
from pathlib import Path
import os
import subprocess
import sys
import tempfile
import unittest
if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS
sys.path.insert(0, str(ROOT / "tools"))
sys.path.insert(0, str(Path(__file__).resolve().parents[1] / "boards"))
from raspberrypi4_fixture import Tree

HARNESS = r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "firmware/device.h"
#include "firmware/irq.h"
#include "firmware/devicetree.h"
#include "boot/context.h"
#include "drivers/core/driver_core.h"
#include "drivers/bus/platform_bus.h"
#include "kernel/arch/common/sunxi_audio.h"
#include "kernel/arch/common/rk3566_audio.h"
#include "kernel/arch/riscv64/platform/sg2002_audio.c"
unsigned host_cpu;int host_trylock_blocked;
struct task_t *proc_current(void){return NULL;}
void *kmalloc(size_t n){return malloc(n);}void *kcalloc(size_t n,size_t z){return calloc(n,z);}
void *krealloc(void*p,size_t n){return realloc(p,n);}void kfree(void*p){free(p);}
void host_log(const char*f,...){(void)f;}void panic(const char*f,...){(void)f;abort();}
uint32_t host_mmio_abort(void){abort();}
void *arch_device_map_mmio(uint64_t p,size_t n,uint32_t f){(void)p;(void)n;(void)f;abort();}
void arch_device_unmap_mmio(void*p,size_t n){(void)p;(void)n;abort();}
int riscv_sg2002_dma_cache_prepare(void){abort();}
void udelay(unsigned n){(void)n;abort();}void mdelay(unsigned n){(void)n;abort();}
int class_device_publish(device_t*d){(void)d;abort();}
void class_device_unpublish(device_t*d){(void)d;}
class_device_t *class_device_ref_for_device(const device_t*d){(void)d;return NULL;}
class_device_t *class_device_get_by_type(uint32_t t,unsigned i){(void)t;(void)i;return NULL;}
int class_device_call_begin(class_device_t*d){(void)d;abort();}
void class_device_call_end(class_device_t*d){(void)d;}void class_device_put(class_device_t*d){(void)d;}
__asm__(".global __driver_init_start\n.global __driver_init_end\n__driver_init_start:\n__driver_init_end:\n");
int main(int argc,char**argv){
 assert(argc==4);int board=atoi(argv[2]),quartz=board==1,nano=board==2,success=atoi(argv[3]);
 FILE*f=fopen(argv[1],"rb");assert(f);assert(!fseek(f,0,SEEK_END));long bytes=ftell(f);rewind(f);
 void*blob=malloc(bytes);assert(fread(blob,1,bytes,f)==(size_t)bytes);fclose(f);
 fw_dt_t tree;fw_node_t root;assert(!fw_dt_init(&tree,blob,bytes));assert(!fw_dt_root(&tree,&root));
 boot_context_t context={.protocol=BOOT_PROTOCOL_FDT,.fdt=blob,.fdt_available=(size_t)bytes,.hardware_root=root};
 assert(!boot_context_publish(&context));
 driver_core_init();int enumerated=firmware_device_enumerate(&root);
 if(enumerated<0){
  assert(!success&&driver_device_count()==0);
  if(nano){sg2002_audio_configure();assert(!device_find_get_by_mmio(0x04130000));}
  else {int rejected=quartz?rk3566_board_prepare_audio():sunxi_board_prepare_audio(SUNXI_AUDIO_A64,1);assert(rejected<0);}
  return 0;
 }
 assert(enumerated>0);
 device_t*dma=device_find_get_by_mmio(nano?0x04330000:(quartz?0xfe550000:0x01c02000));
 device_t*audio=device_find_get_by_mmio(nano?0x04130000:(quartz?0xfe410000:0x01c22c00));
 assert(audio&&!audio->plat_data&&!audio->drv);
 /* Enumeration creates canonical nodes and domains, not RES_IRQ arrays. */
 if(dma){assert(!dma->drv&&!device_get_resource(dma,RES_IRQ,0));}
 int rc;
 if(nano){sg2002_audio_configure();rc=audio->plat_data?0:-ENODEV;}
 else rc=quartz?rk3566_board_prepare_audio():sunxi_board_prepare_audio(SUNXI_AUDIO_A64,1);
 if(success){
  assert(!rc&&audio->plat_data&&dma);
  if(nano){sg2002_audio_platform_t*p=audio->plat_data;assert(p->magic==SG2002_AUDIO_PLATFORM_MAGIC);
   assert(p->dma_irq==29&&p->dma_pa==0x04330000&&p->i2s_pa==0x04130000&&p->dac_pa==0x0300a000&&p->prepare);
   const uint64_t pas[]={0x0300a000,0x04330000,0x03000000,0x03002000,0x03003000,0x04108000,0x03001000};
   const size_t sizes[]={0x100,0x1000,0x1000,0x1000,0x100,0x100,0x1000};
   for(unsigned i=0;i<SG2002_AUDIO_PROVIDERS;i++){
    assert(p->provider_pa[i]==pas[i]&&p->provider_bytes[i]==sizes[i]);
    device_t*d=device_find_get_by_mmio(pas[i]);assert(d&&!d->drv&&!device_get_resource(d,RES_IRQ,0));device_put(d);
   }
  }else if(quartz){rk817_audio_platform_t*p=audio->plat_data;assert(p->magic==RK817_AUDIO_MAGIC);
   assert(p->dma_irq[0]==48&&p->dma_irq[1]==47&&p->dma_pa==0xfe550000&&p->dma_request==2);
  }else{sunxi_audio_platform_t*p=audio->plat_data;assert(p->magic==SUNXI_AUDIO_PLATFORM_MAGIC);
   assert(p->dma_irq==82&&p->dma_pa==0x01c02000&&p->variant==SUNXI_AUDIO_A64);}
 }else assert(rc<0&&!audio->plat_data);
 if(dma){assert(!dma->drv&&!device_get_resource(dma,RES_IRQ,0));device_put(dma);}
 assert(!audio->drv&&!device_get_resource(audio,RES_IRQ,0));device_put(audio);
 return 0;
}
'''


def fixture(quartz=False, *, fault=None):
    t = Tree()
    t.begin(""); t.cells("#address-cells", 2); t.cells("#size-cells", 2)
    t.cells("interrupt-parent", 1)
    t.begin("interrupt-controller")
    t.text("compatible", "arm,gic-v3" if quartz else "arm,gic-400")
    t.cells("phandle", 1); t.cells("#interrupt-cells", 3)
    t.prop("interrupt-controller", b"")
    if fault == "disabled_gic": t.text("status", "disabled")
    t.end()

    def node(name, address, size, compatible, *, irq=None, phandle=None, clock=False):
        t.begin(name); t.text("compatible", compatible)
        t.cells("reg", 0, address, 0, size)
        if irq is not None:
            values = []
            for number in irq: values.extend((2 if fault == "invalid_irq_type" else 0, number, 4))
            if fault == "malformed_irq": values.pop()
            t.cells("interrupts", *values)
        if phandle: t.cells("phandle", phandle)
        if clock: t.cells("#clock-cells", 1)
        if fault == "disabled_dma" and irq is not None: t.text("status", "disabled")
        t.end()

    if not quartz:
        node("audio@1c22c00", 0x01c22c00, 0x200, "allwinner,sun50i-a64-codec-i2s")
        node("dma@1c02000", 0x01c02000, 0x1000, "allwinner,sun50i-a64-dma", irq=[50])
        node("clock@1c20000", 0x01c20000, 0x400, "allwinner,sun50i-a64-ccu")
        node("analog@1f015c0", 0x01f015c0, 4, "allwinner,sun50i-a64-codec-analog")
        node("digital@1c22e00", 0x01c22e00, 0x600, "allwinner,sun50i-a64-codec")
        node("rsb@1f03400", 0x01f03400, 0x40, "allwinner,sun8i-a23-rsb")
    else:
        node("audio@fe410000", 0xfe410000, 0x1000, "rockchip,rk3568-i2s-tdm")
        node("dma@fe550000", 0xfe550000, 0x4000, "arm,pl330", irq=[16, 15])
        node("cru@fdd20000", 0xfdd20000, 0x1000, "rockchip,rk3568-cru", phandle=2, clock=True)
        node("grf@fdc60000", 0xfdc60000, 0x1000, "rockchip,rk3568-grf")
        node("pmucru@fdd00000", 0xfdd00000, 0x1000, "rockchip,rk3568-pmucru")
        node("pmugrf@fdc20000", 0xfdc20000, 0x1000, "rockchip,rk3568-pmugrf")
        t.begin("i2c@fdd40000"); t.text("compatible", "rockchip,rk3568-i2c")
        t.cells("reg", 0, 0xfdd40000, 0, 0x1000)
        t.cells("#address-cells", 1); t.cells("#size-cells", 0)
        t.begin("pmic@20"); t.text("compatible", "rockchip,rk817"); t.cells("reg", 0x20)
        t.cells("#sound-dai-cells", 0); t.text("clock-names", "mclk")
        t.cells("clocks", 2, 71 if fault == "wrong_mclk" else 72)
        t.begin("codec"); t.end(); t.end(); t.end()
    t.end()
    return t.finish([])


def nano_fixture(fault=None):
    """The deployed audio overlay's /soc one-cell register and PLIC topology."""
    t = Tree(); t.begin("")
    t.cells("#address-cells", 2); t.cells("#size-cells", 2)
    t.prop("compatible", b"sipeed,licheerv-nano\0sophgo,sg2002\0")
    t.begin("interrupt-controller"); t.text("compatible", "riscv,plic0")
    t.cells("phandle", 1); t.cells("#interrupt-cells", 1)
    t.prop("interrupt-controller", b""); t.end()
    t.begin("soc"); t.text("compatible", "simple-bus")
    t.cells("#address-cells", 1); t.cells("#size-cells", 1)
    t.prop("ranges", b""); t.cells("interrupt-parent", 1)
    nodes = [
        ("i2s@4130000", 0x04130000, 0x2000, "cvitek,cv1835-i2s"),
        ("audio-codec@300a000", 0x0300a000, 0x100, "cvitek,cv182xadac"),
        ("dma-controller@4330000", 0x04330000, 0x1000, "snps,dmac-bm"),
        ("top-syscon@3000000", 0x03000000, 0x1000, "sophgo,sg2002-top-syscon"),
        ("clock-controller@3002000", 0x03002000, 0x1000, "sophgo,cv1800-clk"),
        ("reset-syscon@3003000", 0x03003000, 0x100, "sophgo,sg2002-reset-syscon"),
        ("i2s-subsystem@4108000", 0x04108000, 0x100, "sophgo,sg2002-i2s-subsystem"),
        ("pinmux-syscon@3001000", 0x03001000, 0x1000, "sophgo,sg2002-pinmux-syscon"),
    ]
    for name, address, size, compatible in nodes:
        t.begin(name)
        if fault == "malformed_provider" and address == 0x0300a000:
            t.prop("compatible", compatible.encode())  # Missing mandatory NUL terminator.
        else: t.text("compatible", compatible)
        if fault == "short_provider" and address == 0x03001000: size = 0x800
        t.cells("reg", address, size)
        if address == 0x03003000 and fault == "disabled_provider": t.text("status", "disabled")
        if address == 0x04330000:
            t.cells("phandle", 2); t.cells("#dma-cells", 3)
            t.cells("dma-channels", 8); t.cells("dma-requests", 16)
            t.cells("dma-masters", 2); t.cells("data-width", 4, 4)
            if fault == "malformed_irq": t.prop("interrupts", b"\0\0\x1d")
            elif fault != "missing_irq": t.cells("interrupts", 29)
        elif address == 0x04130000:
            t.cells("interrupts", 43); t.cells("#sound-dai-cells", 0)
            t.cells("dmas", 2, 7, 1, 1); t.text("dma-names", "tx")
            t.text("capability", "tx"); t.cells("dev-id", 3); t.text("mclk_out", "true")
        t.end()
    t.end(); t.end()
    return t.finish([])


class BoardAudioEnumeration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="a20-audio-enumeration-")
        cls.work = Path(cls.temp.name)
        stubs = dict(STUBS)
        stubs["drivers/core/driver_hwapi.h"] = '''#pragma once
#include "core/types.h"
uint32_t host_mmio_abort(void);
static inline uint32_t readl(const volatile void*p){(void)p;return host_mmio_abort();}
static inline void writel(uint32_t v,volatile void*p){(void)v;(void)p;host_mmio_abort();}
static inline uint64_t readq(const volatile void*p){(void)p;return host_mmio_abort();}
void udelay(unsigned);void mdelay(unsigned);
'''
        for name, content in stubs.items():
            path = cls.work / name; path.parent.mkdir(parents=True, exist_ok=True); path.write_text(content)
        (cls.work / "dma_cache.h").write_text("int riscv_sg2002_dma_cache_prepare(void);\n")
        source = cls.work / "test.c"; source.write_text(HARNESS)
        cls.binary = cls.work / "test"
        sources = ["kernel/core/fdt_view.c", "kernel/firmware/node.c", "kernel/firmware/static.c",
                   "kernel/firmware/irq.c", "kernel/firmware/device.c", "kernel/boot/context.c",
                   "kernel/firmware/devicetree/node.c", "kernel/firmware/devicetree/framebuffer.c",
                   "kernel/firmware/devicetree/memory.c",
                   "kernel/boot/framebuffer.c", "kernel/drivers/core/driver_core.c",
                   "kernel/drivers/bus/platform_bus.c"]
        command = ["cc", "-std=gnu99", "-O1", "-g", "-Wall", "-Wextra", "-Werror",
                   "-fsanitize=address,undefined", "-fno-pie", "-no-pie", "-DCONFIG_NR_CPUS=2",
                   "-I" + str(cls.work), "-I" + str(ROOT / "kernel/include"),
                   "-I" + str(ROOT / "kernel"), "-I" + str(ROOT / "kernel/arch/riscv64/include"),
                   "-I" + str(ROOT), str(source),
                   *[str(ROOT / p) for p in sources], "-o", str(cls.binary)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode: raise AssertionError(result.stderr)

    @classmethod
    def tearDownClass(cls): cls.temp.cleanup()

    def run_fixture(self, quartz=False, fault=None, nano=False):
        path = self.work / "fixture.dtb"; path.write_bytes(nano_fixture(fault) if nano else fixture(quartz, fault=fault))
        result = subprocess.run([str(self.binary), str(path), str(2 if nano else int(quartz)), str(int(fault is None))],
                                capture_output=True, text=True,
                                env={**os.environ, "ASAN_OPTIONS": "detect_leaks=0"}, timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_nano_plic_enumerate_then_metadata(self): self.run_fixture(nano=True)
    def test_nano_disabled_provider_does_not_publish(self): self.run_fixture(nano=True, fault="disabled_provider")
    def test_nano_malformed_provider_does_not_publish(self): self.run_fixture(nano=True, fault="malformed_provider")
    def test_nano_short_provider_does_not_publish(self): self.run_fixture(nano=True, fault="short_provider")
    def test_nano_missing_irq_does_not_publish(self): self.run_fixture(nano=True, fault="missing_irq")
    def test_nano_malformed_irq_does_not_publish(self): self.run_fixture(nano=True, fault="malformed_irq")
    def test_pine_gicv2_enumerate_then_metadata(self): self.run_fixture()
    def test_quartz_gicv3_enumerate_then_metadata(self): self.run_fixture(True)
    def test_missing_domain_does_not_publish(self): self.run_fixture(fault="disabled_gic")
    def test_disabled_dma_does_not_publish(self): self.run_fixture(True, "disabled_dma")
    def test_wrong_codec_clock_does_not_publish(self): self.run_fixture(True, "wrong_mclk")
    def test_malformed_irq_does_not_publish(self): self.run_fixture(True, "malformed_irq")
    def test_invalid_gic_type_does_not_publish(self): self.run_fixture(True, "invalid_irq_type")

if __name__ == "__main__": unittest.main()

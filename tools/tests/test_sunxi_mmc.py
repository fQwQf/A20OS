"""Run the production sunxi PIO transaction engine against register fixtures."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
SCENARIOS = (
    "sdhc_unaligned_multi_sector_read", "sdsc_byte_addressed_write", "no_card",
    "cmd8_voltage_echo", "fifo_timeout", "data_crc_offlines_binding", "bounds",
    "csd_write_protection", "unsupported_csd", "clock_timeout", "card_status_error",
    "invalid_rca", "reject_non_sd_card", "acmd41_power_timeout", "data_timeout",
    "sdxc_maximum_lba", "reject_unsafe_clock_and_config", "transfer_state_timeout",
    "non_error_card_status", "locked_card", "fractional_counter_frequency",
    "reject_incompatible_voltage", "failed_stop_retry", "cancel_pio", "lost_registers",
)


class SunxiMmc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-sunxi-mmc-")
        cls.binary = Path(cls.tmp.name) / "test"
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
            str(ROOT / "kernel/drivers/block/sunxi_mmc_core.c"),
            str(ROOT / "tools/tests/host_sunxi_mmc.c"), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()


def scenario(index):
    def test(self):
        subprocess.run([str(self.binary), str(index)], check=True, timeout=10)
    return test


for index, name in enumerate(SCENARIOS):
    setattr(SunxiMmc, "test_" + name, scenario(index))


# Driver binding fixtures retain the production class adapter and engine;
# allocation, devres registry, MMIO hardware and time are the only substitutes.
BINDING = r'''
#define SUNXI_HOST_NO_MAIN
#include "tools/tests/host_sunxi_mmc.c"
#include "kernel/drivers/block/sunxi_mmc.c"
#include <stdarg.h>
static hardware_t hardware;
static int allocations, fail_allocation, fail_action, fail_map, read_only, managed, cancel_binding;
static int (*cleanup)(void *);
static void *cleanup_context;
int host_trylock_blocked;
uint32_t host_readl(const volatile void *p) {
 uint32_t value=read_register(&hardware,(uintptr_t)p-(uintptr_t)hardware.registers);
 if(cancel_binding&&hardware.fifo_words==16) {
  cancel_binding=0;assert(((sunxi_mmc_binding_t*)cleanup_context)->lock.depth==1);
  assert(stop_binding(cleanup_context)==-EBUSY&&allocations==1);
 }
 return value;
}
void host_writel(uint32_t v,volatile void *p) {
 write_register(&hardware,(uintptr_t)p-(uintptr_t)hardware.registers,v);
}
uint64_t clock_get_ticks(void) { return ticks(&hardware); }
uint64_t clock_ticks_per_sec(void) { return 1000; }
void *kcalloc(size_t n,size_t s) { if(fail_allocation)return NULL;allocations++;return calloc(n,s); }
void kfree(void *data) {
 sunxi_mmc_binding_t *p=data;
 assert(allocations>0&&p->host.closing&&p->hardware_stopped&&!p->lock.depth);
 allocations--;free(p);
}
void host_log(const char *fmt,...) { (void)fmt; }
static resource_t registers={.type=RES_MMIO,.start=0x1000,.end=0x1fff};
resource_t *device_get_resource(device_t *d,enum resource_type type,int index) {
 (void)d; assert(type==RES_MMIO&&index==0);return &registers;
}
void *device_map_mmio(device_t *d,int index) {
 assert(index==0&&managed&&cleanup&&d->drv_priv);return fail_map?NULL:hardware.registers;
}
int device_probe_use_remove(device_t *d) {(void)d;managed=1;return 0;}
int device_add_execution_action(device_t *d,int (*action)(void *),void (*release)(void *),void *context) {
 assert(managed&&d->drv_priv==context&&!release);
 if(fail_action)return -ENOMEM;
 assert(!cleanup);cleanup=action;cleanup_context=context;return 0;
}
int fw_node_has_property(const fw_node_t *n,const char *name) {
 (void)n;assert(!strcmp(name,"read-only"));return read_only;
}
bus_type_t platform_bus;
static int release_resources(device_t *dev) {
 int rc=cleanup?cleanup(cleanup_context):0;
 if(!rc)rc=sunxi_remove(dev);
 if(!rc){cleanup=NULL;cleanup_context=NULL;assert(!allocations);}
 return rc;
}
static void closed_io(device_t *dev) {
 unsigned reads=hardware.mmio_reads,writes=hardware.mmio_writes;uint8_t data[512];
 assert(sunxi_read(dev,0,data,1)==-ENODEV&&sunxi_write(dev,0,data,1)==-ENODEV);
 assert(sunxi_flush(dev)==-ENODEV&&!sunxi_capacity(dev));
 assert(reads==hardware.mmio_reads&&writes==hardware.mmio_writes);
}
int main(int argc,char **argv) {
 assert(argc==2);int scenario=atoi(argv[1]); init(&hardware);
 /* Refer to the shared fixture helper so -Werror still checks every source. */
 sunxi_mmc_host_t dummy; assert(!start(&hardware,&dummy));init(&hardware);
 device_t device={.name="mmc0",.plat_data=&platform};
 if(scenario==0) {
  assert(!sunxi_probe(&device));assert(device.drv_priv&&allocations==1);
  assert(sunxi_capacity(&device)==74566ULL*1024&&sunxi_sector_size(&device)==512);
  uint8_t data[512];assert(!sunxi_read(&device,4,data,1));assert(hardware.arguments[0]==4);
  assert(!sunxi_flush(&device));assert(!release_resources(&device));
  assert(!device.drv_priv&&sunxi_read(&device,0,data,1)==-ENODEV);
  assert(!hardware.registers[0x04/4]);
 } else if(scenario==1) {
  hardware.no_card=1;assert(sunxi_probe(&device)==-ETIMEDOUT);
  assert(device.drv_priv&&allocations==1);assert(!release_resources(&device));assert(!hardware.registers[0x04/4]);
 } else if(scenario==2) {
  fail_action=1;assert(sunxi_probe(&device)==-ENOMEM&&device.drv_priv);assert(!release_resources(&device));
 } else if(scenario==3) {
  fail_allocation=1;assert(sunxi_probe(&device)==-ENOMEM);assert(!release_resources(&device));
 } else if(scenario==4) {
  device.plat_data=NULL;assert(sunxi_probe(&device)==-ENODEV);assert(!release_resources(&device));
 } else if(scenario==5) {
  read_only=1;assert(!sunxi_probe(&device));uint8_t data[512];
  assert(sunxi_write(&device,0,data,1)==-EROFS&&!hardware.write_commands);
  assert(!release_resources(&device));
 } else if(scenario==6) {
  read_only=-EINVAL;assert(sunxi_probe(&device)==-EINVAL);
  assert(device.drv_priv&&allocations==1);assert(!release_resources(&device));assert(!hardware.registers[0x04/4]);
 } else if(scenario==7) {
  for(unsigned failure=0;failure<6;failure++) {
   init(&hardware);assert(!sunxi_probe(&device));void *held=device.drv_priv;
   hardware.clock_stuck=failure==0;hardware.reset_stuck=failure==1;
   hardware.mask_stuck=failure==2;hardware.disable_stuck=failure==3;hardware.gone=failure==4;
   hardware.clock_error=failure==5;
   if(hardware.mask_stuck)hardware.registers[0x30/4]=1;
   assert(release_resources(&device)==(hardware.gone?-ENODEV:hardware.clock_error?-EIO:-ETIMEDOUT));
   assert(device.drv_priv==held&&allocations==1);closed_io(&device);
   hardware.clock_stuck=hardware.reset_stuck=hardware.mask_stuck=hardware.disable_stuck=hardware.gone=hardware.clock_error=0;
   assert(!release_resources(&device)&&!device.drv_priv);
  }
 } else if(scenario==8) {
  assert(!sunxi_probe(&device));sunxi_mmc_binding_t *p=device.drv_priv;
  mutex_lock(&p->lock);assert(stop_binding(p)==-EBUSY&&allocations==1);mutex_unlock(&p->lock);
  closed_io(&device);assert(!release_resources(&device));
 } else if(scenario==9) {
  assert(!sunxi_probe(&device));cancel_binding=1;uint8_t data[512];
  assert(sunxi_read(&device,0,data,1)==-ENODEV&&hardware.fifo_words==16&&allocations==1);
  closed_io(&device);assert(!release_resources(&device));
 } else if(scenario==10) {
  hardware.clock_stuck=1;assert(sunxi_probe(&device)==-ETIMEDOUT);
  void *held=device.drv_priv;assert(held&&release_resources(&device)==-ETIMEDOUT);
  assert(device.drv_priv==held&&allocations==1);hardware.clock_stuck=0;
  assert(!release_resources(&device));
 } else if(scenario==11) {
  fail_map=1;assert(sunxi_probe(&device)==-EIO&&device.drv_priv&&allocations==1);
  assert(!release_resources(&device)&&!hardware.mmio_writes);
 } else assert(!"invalid scenario");
 assert(!allocations&&!device.drv_priv&&!sunxi_remove(&device));return 0;
}
'''


class SunxiBinding(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if __package__:
            from .test_firmware_model import STUBS
        else:
            from test_firmware_model import STUBS
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-sunxi-binding-")
        work = Path(cls.tmp.name)
        for name in ("core/defs.h", "core/lock.h", "core/sync.h", "core/klog.h"):
            file = work / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(STUBS[name])
        header = work / "drivers/core/driver_hwapi.h"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text('''#pragma once
#include "core/types.h"
uint32_t host_readl(const volatile void *);
void host_writel(uint32_t,volatile void *);
#define readl host_readl
#define writel host_writel
uint64_t clock_get_ticks(void);
uint64_t clock_ticks_per_sec(void);
''')
        (work / "binding.c").write_text(BINDING)
        cls.binary = work / "test"
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-I" + str(work), "-I" + str(ROOT),
            "-I" + str(ROOT / "kernel/include"), "-I" + str(ROOT / "kernel"),
            str(ROOT / "kernel/drivers/block/sunxi_mmc_core.c"),
            str(work / "binding.c"), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()


for index, name in enumerate(("class_remove", "probe_no_card_rollback", "action_failure",
                             "allocation_failure", "unprepared_board", "read_only", "property_failure",
                             "failed_stop_retains_binding", "busy_stop", "cancel_inflight_pio",
                             "failed_probe_retained_until_retry", "mapping_failure")):
    setattr(SunxiBinding, "test_" + name, scenario(index))


BOARD = r'''
#include <assert.h>
#include <string.h>
#include <stdlib.h>
#include "arch/common/sunxi_mmc0.h"
static uint32_t ccu[256], pio[64];
static device_t device;
static int mappings, unmaps, held, missing, wrong_id, map_failure;
uint32_t host_readl(const volatile void *p) { return *(const volatile uint32_t *)p; }
void host_writel(uint32_t value,volatile void *p) { *(volatile uint32_t *)p=value; }
device_t *device_find_get_by_mmio(uint64_t physical) {
 assert(physical==0x01c0f000);if(missing)return NULL;held++;return &device;
}
void device_put(device_t *d) { assert(d==&device&&held==1);held--; }
int fw_node_match_id(const fw_node_t *n,const char *id) {
 (void)n;assert(!strcmp(id,"allwinner,sun50i-a64-mmc")||!strcmp(id,"allwinner,sun7i-a20-mmc"));return !wrong_id;
}
void *arch_device_map_mmio(uint64_t physical,size_t length,uint32_t flags) {
 assert(!flags);mappings++;
 if(physical==0x01c20000){assert(length==sizeof(ccu));return ccu;}
 assert(physical==0x01c20800&&length==sizeof(pio));return map_failure?NULL:pio;
}
void arch_device_unmap_mmio(void *p,size_t length) {
 assert((p==ccu&&length==sizeof(ccu))||(p==pio&&length==sizeof(pio)));unmaps++;
}
int main(int argc,char **argv) {
 assert(argc==2);int test=atoi(argv[1]);memset(ccu,0xa5,sizeof(ccu));memset(pio,0xa5,sizeof(pio));
 missing=test==2;wrong_id=test==3;map_failure=test==4;
 sunxi_board_prepare_mmc0(test==0);
 assert(!held);
 if(missing||wrong_id){assert(!mappings&&!device.plat_data);return 0;}
 if(map_failure){assert(mappings==2&&unmaps==1&&!device.plat_data);return 0;}
 assert(mappings==2&&unmaps==2&&device.plat_data);
 const sunxi_mmc_platform_t *p=device.plat_data;
 assert(p->clock_hz==400000&&p->fifo_offset==0x200&&p->a64_timing==(test==0));
 assert(ccu[0x88/4]==((1U<<31)|((test==0?1U:2U)<<16)|14U));
 assert(ccu[0x60/4]==(0xa5a5a5a5U|(1U<<8)));
 assert(ccu[0x2c0/4]==(0xa5a5a5a5U|(1U<<8)));
 assert(pio[0xb4/4]==((0xa5a5a5a5U&~0xffffffU)|0x222222U));
 assert(pio[0xc8/4]==((0xa5a5a5a5U&~0xfffU)|0xaaaU));
 assert(pio[0xd0/4]==((0xa5a5a5a5U&~0xfffU)|0x555U));
 assert(pio[0xb0/4]==0xa5a5a5a5U&&pio[0xd4/4]==0xa5a5a5a5U);
 return 0;
}
'''


class SunxiBoard(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        if __package__:
            from .test_firmware_model import STUBS
        else:
            from test_firmware_model import STUBS
        cls.tmp = tempfile.TemporaryDirectory(prefix="a20-sunxi-board-")
        work = Path(cls.tmp.name)
        for name in ("core/defs.h", "core/lock.h", "core/stdio.h"):
            file = work / name
            file.parent.mkdir(parents=True, exist_ok=True)
            file.write_text(STUBS[name])
        header = work / "drivers/core/driver_hwapi.h"
        header.parent.mkdir(parents=True, exist_ok=True)
        header.write_text("""#pragma once
#include "core/types.h"
uint32_t host_readl(const volatile void *);
void host_writel(uint32_t,volatile void *);
#define readl host_readl
#define writel host_writel
""")
        (work / "board.c").write_text(BOARD)
        cls.binary = work / "test"
        subprocess.run([
            "cc", "-std=gnu99", "-Wall", "-Wextra", "-Werror", "-g",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer", "-no-pie",
            "-I" + str(work), "-I" + str(ROOT / "kernel/include"),
            "-I" + str(ROOT / "kernel"), str(work / "board.c"), "-o", str(cls.binary),
        ], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.tmp.cleanup()


for index, name in enumerate(("a64_fixed_postdivider", "v3s_legacy_divider", "no_device",
                             "wrong_controller", "mapping_failure")):
    setattr(SunxiBoard, "test_" + name, scenario(index))


if __name__ == "__main__":
    unittest.main()

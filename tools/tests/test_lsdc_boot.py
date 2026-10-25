"""Execute the production LS2K0300 retained-plane driver against a DC model.

This checks admission/ownership and buffer arithmetic, not panel signal timing.
"""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
STUB = r'''
#ifndef LSDC_TEST_STUB
#define LSDC_TEST_STUB
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <errno.h>
#define RES_MMIO 1
#define DEV_CLASS_DISPLAY 3
#define DRIVER_REGISTER(d) static const void *registered_driver=&d
static int platform_bus;
typedef struct {int unused;} fw_node_t;
typedef struct {uint64_t start,end;unsigned flags;} resource_t;
typedef struct device {void *drv_priv,*plat_data;fw_node_t firmware_node;resource_t resource;} device_t;
typedef struct {const char *id;} firmware_device_id_t;
typedef struct {unsigned stride,red_mask,green_mask,blue_mask,reserved_mask;} gpu_scanout_layout_t;
typedef struct {int(*get_info)(device_t*,uint32_t*,uint32_t*,uint32_t*);int(*get_fb)(device_t*,uintptr_t*,size_t*);int(*get_layout)(device_t*,gpu_scanout_layout_t*);int(*flush)(device_t*,uint32_t,uint32_t,uint32_t,uint32_t);} gpu_dev_ops_t;
typedef struct {const char*name;void*bus;const firmware_device_id_t*firmware_ids;int(*probe)(device_t*);int(*remove)(device_t*);int class_type;const gpu_dev_ops_t*class_ops;} driver_t;
static unsigned allocations,frees,maps,unmaps,registrations,unregisters,barriers;
static int fail_alloc,fail_map,register_result,property_result;
static inline void *kcalloc(size_t n,size_t size){if(fail_alloc)return NULL;allocations++;return calloc(n,size);}
static inline void kfree(void *p){assert(p);frees++;free(p);}
static inline void wmb(void){barriers++;}
uint32_t readl(const volatile void*);
void *arch_device_map_mmio(uint64_t,size_t,uint32_t);
void arch_device_unmap_mmio(void*,size_t);
resource_t *device_get_resource(device_t*,int,int);
int fw_node_read_u64_array(const fw_node_t*,const char*,uint64_t*,size_t);
int gpu_device_register(device_t*);
void gpu_device_unregister(device_t*);
#endif
'''
HARNESS = r'''
#include "stub.h"
#include "PRODUCTION"
static uint32_t registers[0x2000/4];
static uint8_t scanout;
static unsigned config_reads;
static int unstable;
static uint64_t aperture[2]={0x08000000,0x07000000};
static int prepared,prepare_result;
static int prepare_scanout(uint64_t pa,size_t bytes){
 assert(pa>=0x08000000 && bytes && bytes<=0x0f000000-pa);
 prepared++;return prepare_result;
}
static lsdc_boot_policy_t default_policy;
static device_t device={.resource={0x16090000,0x16091fff,0}};
uint32_t readl(const volatile void *p){
 const volatile uint32_t *r=p;
 if(r==&registers[LSDC_BOOT_CFG/4] && ++config_reads==2 && unstable)
  return *r^LSDC_BOOT_INDEX;
 return *r;
}
void *arch_device_map_mmio(uint64_t pa,size_t size,uint32_t flags){
 (void)flags;
 if(fail_map && maps+1==(unsigned)fail_map){maps++;return NULL;}
 maps++;
 if(pa==0x16090000){assert(size==LSDC_BOOT_REG_BYTES);return registers;}
 assert(prepared);
 assert(pa>=aperture[0] && pa+size<=aperture[0]+aperture[1]);return &scanout;
}
void arch_device_unmap_mmio(void *p,size_t size){assert(p && size);unmaps++;}
resource_t *device_get_resource(device_t*d,int type,int index){assert(type==RES_MMIO&&!index);return &d->resource;}
int fw_node_read_u64_array(const fw_node_t*n,const char*name,uint64_t*out,size_t count){
 (void)n;assert(!strcmp(name,"a20,scanout-aperture")&&count==2);
 memcpy(out,aperture,sizeof(aperture));return property_result;
}
int gpu_device_register(device_t*d){assert(d->drv_priv);registrations++;return register_result;}
void gpu_device_unregister(device_t*d){assert(d->drv_priv);unregisters++;}
static void defaults(void){
 assert(!device.drv_priv);device.plat_data=NULL;memset(registers,0,sizeof(registers));
 registers[LSDC_BOOT_CFG/4]=LSDC_BOOT_OUTPUT|LSDC_BOOT_RESET|4;
 registers[LSDC_BOOT_HDISPLAY/4]=(1056<<16)|800;
 registers[LSDC_BOOT_VDISPLAY/4]=(628<<16)|600;
 registers[LSDC_BOOT_STRIDE/4]=3328;
 registers[LSDC_BOOT_ADDR0/4]=0x0dc00000;
 registers[LSDC_BOOT_ADDR1/4]=0x0e000000;
 config_reads=0;unstable=fail_alloc=fail_map=register_result=property_result=0;
 allocations=frees=maps=unmaps=registrations=unregisters=barriers=0;
 aperture[0]=0x08000000;aperture[1]=0x07000000;
 default_policy=(lsdc_boot_policy_t){LSDC_BOOT_POLICY_MAGIC,LSDC_BOOT_POLICY_VERSION,
                                    0x16090000,aperture[0],aperture[1],prepare_scanout};
 device.plat_data=&default_policy;prepared=prepare_result=0;
}
static void rejected(unsigned off,uint32_t value){
 defaults();registers[off/4]=value;
 assert(probe(&device)<0 && !device.drv_priv && !registrations);
 assert(allocations==frees && maps==unmaps);
}
int main(void){
 (void)registered_driver;
 rejected(LSDC_BOOT_CFG,LSDC_BOOT_RESET|4); /* inactive */
 rejected(LSDC_BOOT_CFG,LSDC_BOOT_OUTPUT|4); /* reset */
 rejected(LSDC_BOOT_ORIGIN,1);
 rejected(LSDC_BOOT_CFG,LSDC_BOOT_OUTPUT|LSDC_BOOT_RESET|4|(1<<7));
 rejected(LSDC_BOOT_CFG,LSDC_BOOT_OUTPUT|LSDC_BOOT_RESET|4|(1<<9));
 rejected(LSDC_BOOT_CFG,LSDC_BOOT_OUTPUT|LSDC_BOOT_RESET|4|(1U<<31));
 rejected(LSDC_BOOT_CFG,LSDC_BOOT_OUTPUT|LSDC_BOOT_RESET|2);
 rejected(LSDC_BOOT_HDISPLAY,(1056<<16));
 rejected(LSDC_BOOT_HDISPLAY,(1056<<16)|4097);
 rejected(LSDC_BOOT_VDISPLAY,(600<<16)|600);
 rejected(LSDC_BOOT_STRIDE,3201);
 rejected(LSDC_BOOT_STRIDE,256);
 rejected(LSDC_BOOT_ADDR0,0x0dc00001);
 rejected(LSDC_BOOT_ADDR0,0x07fff000); /* allocator overlap */
 rejected(LSDC_BOOT_ADDR0,0x0efff000); /* mapping crosses upper bound */
 rejected(LSDC_BOOT_HI0,1);
 defaults();aperture[0]=UINT64_MAX-4095;aperture[1]=8192;
 default_policy.aperture_base=aperture[0];default_policy.aperture_bytes=aperture[1];
 assert(probe(&device)==-ENODEV && !registrations && allocations==frees);
 defaults();property_result=-ENOENT;
 lsdc_boot_policy_t policy={LSDC_BOOT_POLICY_MAGIC,LSDC_BOOT_POLICY_VERSION,
                            0x16090000,0x08000000,0x07000000,prepare_scanout};
 device.plat_data=&policy;
 assert(!probe(&device));assert(!remove_device(&device));
 assert(device.plat_data==&policy); /* borrowed policy survives remove */
 defaults();device.plat_data=&policy;policy.magic=0;
 assert(probe(&device)==-EINVAL&&!maps);policy.magic=LSDC_BOOT_POLICY_MAGIC;
 defaults();device.plat_data=&policy;policy.version++;
 assert(probe(&device)==-EINVAL&&!maps);policy.version=LSDC_BOOT_POLICY_VERSION;
 defaults();device.plat_data=&policy;policy.register_base+=0x10000;
 assert(probe(&device)==-EINVAL&&!maps);policy.register_base=0x16090000;
 defaults();device.plat_data=&policy;policy.aperture_base=0x0e800000;
 assert(probe(&device)==-ENODEV&&!registrations);policy.aperture_base=0x08000000;
 defaults();unstable=1;assert(probe(&device)==-ENODEV&&!registrations);
 defaults();device.plat_data=NULL;assert(probe(&device)==-ENODEV&&!maps);
 defaults();default_policy.prepare=NULL;assert(probe(&device)==-EINVAL&&!maps);
 defaults();prepare_result=-EIO;assert(probe(&device)==-EIO&&prepared==1&&maps==1&&unmaps==1);
 defaults();fail_alloc=1;assert(probe(&device)==-ENOMEM&&!maps);
 defaults();fail_map=1;assert(probe(&device)==-ENXIO&&allocations==frees);
 defaults();fail_map=2;assert(probe(&device)==-ENXIO&&unmaps==1&&allocations==frees);
 defaults();register_result=-ENOMEM;
 assert(probe(&device)==-ENOMEM&&!device.drv_priv&&maps==unmaps&&allocations==frees);
 for(unsigned index=0;index<2;index++){
  defaults();registers[LSDC_BOOT_CFG/4]|=index?LSDC_BOOT_INDEX:0;
  if(index)registers[LSDC_BOOT_ADDR0/4]=1; /* ignore inactive buffer */
  uint32_t before[0x2000/4];memcpy(before,registers,sizeof(before));
  assert(!probe(&device)&&registrations==1);
  uint32_t w,h,bpp;uintptr_t physical;size_t bytes;gpu_scanout_layout_t l;
  assert(!info(&device,&w,&h,&bpp)&&w==800&&h==600&&bpp==32);
  assert(!framebuffer(&device,&physical,&bytes));
  assert(physical==(index?0x0e000000:0x0dc00000)&&bytes==1998848);
  assert(!layout(&device,&l)&&l.stride==3328&&l.red_mask==0xff0000);
  assert(!flush(&device,0,0,w,h)&&barriers==1);
  assert(flush(&device,800,0,1,1)==-EINVAL);
  assert(flush(&device,UINT32_MAX,0,1,1)==-EINVAL);
  registers[LSDC_BOOT_STRIDE/4]+=256;assert(flush(&device,0,0,1,1)==-ENODEV);
  registers[LSDC_BOOT_STRIDE/4]-=256;
  assert(!remove_device(&device)&&!device.drv_priv&&unregisters==1);
  assert(allocations==frees&&maps==unmaps&&!memcmp(before,registers,sizeof(before)));
  assert(!remove_device(&device)&&unregisters==1); /* idempotent, no HW writes */
 }
 defaults();registers[LSDC_BOOT_CFG/4]=(registers[LSDC_BOOT_CFG/4]&~7U)|3;
 registers[LSDC_BOOT_STRIDE/4]=1792;
 assert(!probe(&device));gpu_scanout_layout_t l;
 assert(!layout(&device,&l)&&l.red_mask==0xf800&&l.green_mask==0x07e0&&l.blue_mask==31&&!l.reserved_mask);
 assert(!remove_device(&device));
 puts("LSDC retained scanout model: PASS");return 0;
}
'''

BOARD_MODEL = r'''
#include "stub.h"
#include "HEADER"
static device_t canonical;
static int exists,match,enumerations,puts_count;
static const int display_tree;
device_t *device_find_get_by_mmio(uint64_t pa){assert(pa==0x16090000);return exists?&canonical:NULL;}
resource_t *device_get_resource(device_t*d,int type,int index){assert(type==RES_MMIO&&!index);return &d->resource;}
int fw_node_match_id(const fw_node_t*n,const char*id){(void)n;assert(!strcmp(id,"loongson,la2k0300-dc")||!strcmp(id,"a20,ls2k0300-boot-display"));return match;}
void device_put(device_t*d){assert(d==&canonical);puts_count++;}
void board_firmware_static_enumerate(const int*t){assert(t==&display_tree);enumerations++;exists=match=1;canonical.resource=(resource_t){0x16090000,0x16091fff,0};}
int ls2k0300_scanout_prepare(uint64_t pa,size_t bytes){(void)pa;(void)bytes;return 0;}
POLICY
CONFIGURE
int main(void){
 (void)platform_bus;(void)allocations;(void)frees;(void)maps;(void)unmaps;
 (void)registrations;(void)unregisters;(void)barriers;(void)fail_map;
 (void)register_result;(void)property_result;
 configure_devices();assert(enumerations==1&&puts_count==1&&canonical.plat_data==&display_policy);
 exists=1;match=1;canonical.resource=(resource_t){0x16090000,0x16091fff,0};
 configure_devices();assert(canonical.plat_data==&display_policy&&enumerations==1&&puts_count==2);
 assert(display_policy.aperture_base==0x08000000&&display_policy.aperture_bytes==0x07000000);
 canonical.plat_data=NULL;match=0;configure_devices();assert(!canonical.plat_data&&enumerations==1);
 match=-EINVAL;configure_devices();assert(!canonical.plat_data);
 match=1;canonical.resource.start++;configure_devices();assert(!canonical.plat_data);
 canonical.resource.start=0x16090000;canonical.resource.end=0x160900ff;
 configure_devices();assert(!canonical.plat_data);
 canonical.resource.end=0x16091fff;int foreign=1;canonical.plat_data=&foreign;
 configure_devices();assert(canonical.plat_data==&foreign);
 return 0;
}
'''

class LsdcBootModel(unittest.TestCase):
    def test_production_board_attaches_policy_only_to_matching_canonical_device(self):
        board = (ROOT / "kernel/arch/loongarch64/boards/ls2k0300/board.c").read_text()
        begin = board.index("static const lsdc_boot_policy_t display_policy =")
        policy = board[begin:board.index("};", begin) + 2]
        begin = board.index("static void configure_devices(void)")
        configure = board[begin:board.index("\n}\n", begin) + 3]
        model = BOARD_MODEL.replace("HEADER", str(ROOT / "kernel/drivers/gpu/lsdc_boot.h"))
        model = model.replace("POLICY", policy).replace("CONFIGURE", configure)
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "stub.h").write_text(STUB)
            (stage / "core").mkdir()
            (stage / "core/types.h").write_text('#include "stub.h"\n')
            (stage / "model.c").write_text(model)
            build = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-I", str(stage), str(stage / "model.c"), "-o", str(stage / "model")],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(stage / "model")], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stderr)

    def test_production_driver_register_and_ownership_model(self):
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            (stage / "stub.h").write_text(STUB)
            headers = ("drivers/gpu/gpu_core.h", "drivers/core/driver_register.h",
                       "drivers/core/driver_hwapi.h", "drivers/bus/platform_bus.h",
                       "core/types.h", "core/errno.h", "core/stdio.h", "core/string.h", "mm/slab.h")
            for header in headers:
                target = stage / header
                target.parent.mkdir(parents=True, exist_ok=True)
                target.write_text('#include "stub.h"\n')
            production = ROOT / "kernel/drivers/gpu/lsdc_boot.c"
            source = stage / "model.c"
            source.write_text(HARNESS.replace("PRODUCTION", str(production)))
            binary = stage / "model"
            build = subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                                    "-fsanitize=undefined", "-fno-sanitize-recover=all",
                                    "-I", str(stage), str(source), "-o", str(binary)],
                                   capture_output=True, text=True)
            self.assertEqual(build.returncode, 0, build.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("LSDC retained scanout model: PASS", result.stdout)

if __name__ == "__main__":
    unittest.main()

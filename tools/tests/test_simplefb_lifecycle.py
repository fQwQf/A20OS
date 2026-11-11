"""Real simplefb probe/remove and firmware-provider retention lifecycle."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest
if __package__:
    from .test_firmware_model import ROOT, STUBS
else:
    from test_firmware_model import ROOT, STUBS
HARNESS=r'''
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "firmware/irq.h"
#include "kernel/drivers/gpu/simplefb.c"
unsigned host_cpu;int host_trylock_blocked;
struct task_t *proc_current(void){return NULL;}
static int mapping_failure,flush_failure,gpus,provider_probes,provider_removes;
static unsigned char scanout[4096];
void *kmalloc(size_t n){return malloc(n);}void *kcalloc(size_t n,size_t z){return calloc(n,z);}
void *krealloc(void*p,size_t n){return realloc(p,n);}void kfree(void*p){free(p);}
void panic(const char *fmt,...){(void)fmt;abort();}void host_log(const char *fmt,...){(void)fmt;}
void *arch_device_map_mmio(uint64_t p,size_t n,uint32_t f){(void)p;(void)n;(void)f;return NULL;}
void arch_device_unmap_mmio(void*p,size_t n){(void)p;(void)n;}
const char *bootargs_get(void){return flush_failure?"simplefb.test=1":NULL;}
void *boot_framebuffer_map(void){return mapping_failure?NULL:scanout;}
int boot_framebuffer_flush(void){return flush_failure?-EIO:0;}
int gpu_device_register(device_t*d){(void)d;gpus++;return 0;}
void gpu_device_unregister(device_t*d){(void)d;gpus--;}
int class_device_publish(device_t*d){(void)d;return 0;}
void class_device_mark_offline(device_t *d) { (void)d; }
void class_device_unpublish(device_t*d){(void)d;}
int firmware_irq_domains_prepare(const fw_node_t*n,fw_irq_domain_t**d){(void)n;*d=NULL;return 0;}
void firmware_irq_domains_abort(fw_irq_domain_t*d){(void)d;}
int fw_irq_resolve(const fw_reference_t*r,uint32_t*i,uint32_t*f){(void)r;(void)i;(void)f;return -2;}
__asm__(".global __driver_init_start\n.global __driver_init_end\n__driver_init_start:\n__driver_init_end:\n");
static const char fb_id[]="simple-framebuffer",clock_id[]="test,clock";
static const fw_static_property_t fbprops[]={{"compatible",{fb_id,sizeof(fb_id),FW_ORDER_NATIVE}}};
static const fw_static_property_t clockprops[]={{"compatible",{clock_id,sizeof(clock_id),FW_ORDER_NATIVE}}};
static const uint32_t argument[]={5};
static const fw_static_reference_t references[]={
 {"clocks",1,argument,1},{"clocks",1,argument,1},{"resets",1,argument,1}};
static const fw_resource_t resource={FW_RESOURCE_MEMORY,0x47000000,4096,0,"scanout"};
static fw_static_node_t fixture_nodes[]={
 {.name="root",.parent=SIZE_MAX},
 {.name="clock",.parent=0,.properties=clockprops,.property_count=1},
 {.name="display",.parent=0,.properties=fbprops,.property_count=1,
 .resources=&resource,.resource_count=1,.references=references,.reference_count=3},
};
static const fw_static_tree_t description={fixture_nodes,3};
static int native_probe(device_t*d){(void)d;provider_probes++;return 0;}
static int native_remove(device_t*d){(void)d;provider_removes++;return 0;}
static const firmware_device_id_t provider_ids[]={{.id="test,clock"},{0}};
static driver_t native={.name="native-clock",.bus=&platform_bus,.firmware_ids=provider_ids,
 .probe=native_probe,.remove=native_remove};
int main(int argc,char **argv){
 assert(argc==2);int scenario=atoi(argv[1]);mapping_failure=scenario==1;flush_failure=scenario==4;
 driver_core_init();fw_node_t root,selected,clock;
 assert(!fw_static_init(&description,&root));assert(!fw_static_get(&description,2,&selected));
 assert(!fw_static_get(&description,1,&clock));
 boot_context_t context={.protocol=BOOT_PROTOCOL_FDT,.framebuffer_node=selected,
 .framebuffer_memory=BOOT_FRAMEBUFFER_RAM,
 .framebuffer={BOOT_FRAMEBUFFER_MAGIC,BOOT_FRAMEBUFFER_VERSION,0x47000000,4096,4,4,16,32,
               0xff0000,0xff00,0xff,0xff000000}};
 assert(!boot_context_publish(&context));
 assert(firmware_device_enumerate(&root)==2);
 device_t *provider=firmware_device_find(&clock),*display=firmware_device_find(&selected);
 assert(provider&&display);unsigned before=provider->references;
 if(scenario==2)assert(!driver_register(&native));
 assert(!driver_register(&simplefb_driver));
 if(scenario==0 || scenario==3){
  assert(display->drv==&simplefb_driver&&gpus==1);
  /* Three DT references to one provider retain exactly one graph pin. */
  assert(provider->references==before+1);
  assert(!driver_register(&native));assert(!provider_probes&&!provider->drv);
  assert(provider->probe_error==-EPROBE_DEFER);
  if(scenario==0){assert(!driver_unregister(&simplefb_driver));
   assert(gpus==0&&provider_probes==1&&provider->drv==&native);
   assert(provider->references==before);
  }else{device_unregister(provider);assert(gpus==0&&!display->drv);}
 }else if(scenario==1||scenario==4){
  assert(!display->drv&&!gpus&&provider->references==before);
  assert(!driver_register(&native));assert(provider_probes==1&&provider->drv==&native);
 }else{assert(!display->drv&&!gpus&&display->probe_error==-EBUSY);assert(provider_probes==1);}
 if(display->drv)driver_unregister(&simplefb_driver);
 if(provider->drv)driver_unregister(&native);
 device_unregister(display);
 if(scenario!=3)device_unregister(provider);
 device_put(display);device_put(provider);
 return 0;
}
'''
class SimplefbLifecycle(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-simplefb-lifecycle-');work=Path(cls.temp.name)
        stubs=dict(STUBS);stubs['drivers/core/driver_register.h']='#define DRIVER_REGISTER(driver)\n'
        for name,text in stubs.items():
            path=work/name;path.parent.mkdir(parents=True,exist_ok=True);path.write_text(text)
        (work/'test.c').write_text(HARNESS)
        sources=['kernel/drivers/core/driver_core.c','kernel/drivers/bus/platform_bus.c',
                 'kernel/firmware/node.c','kernel/firmware/static.c','kernel/firmware/device.c',
                 'kernel/boot/context.c','kernel/boot/framebuffer.c']
        cls.binary=work/'test'
        run=subprocess.run(['cc','-std=gnu99','-O1','-g','-Wall','-Wextra','-Werror',
            '-DCONFIG_NR_CPUS=2','-ffunction-sections','-fdata-sections',
            '-fsanitize=address,undefined','-fno-sanitize-recover=all',
            '-I'+str(work),'-I'+str(ROOT),'-I'+str(ROOT/'kernel/include'),'-I'+str(ROOT/'kernel'),
            str(work/'test.c'),*[str(ROOT/p) for p in sources],'-Wl,--gc-sections',
            '-o',str(cls.binary)],capture_output=True,text=True)
        if run.returncode:raise RuntimeError(run.stderr)
    @classmethod
    def tearDownClass(cls):cls.temp.cleanup()
    def scenario(self,n):
        run=subprocess.run([str(self.binary),str(n)],capture_output=True,text=True,timeout=10,
                           env={**os.environ,'ASAN_OPTIONS':'detect_leaks=0'})
        self.assertEqual(run.returncode,0,run.stdout+run.stderr)
    def test_retained_clock_defers_native_probe_and_retries_after_remove(self):self.scenario(0)
    def test_probe_failure_releases_all_holds(self):self.scenario(1)
    def test_native_provider_requires_explicit_handoff(self):self.scenario(2)
    def test_provider_withdraw_removes_display_first(self):self.scenario(3)
    def test_flush_failure_does_not_leave_a_default_gpu(self):self.scenario(4)
if __name__=='__main__':unittest.main()

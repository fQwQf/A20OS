/* Production driver, with PCI/MMIO/device completion and core order modeled. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096
#define ALIGNED(n) __attribute__((aligned(n)))
#define VENDOR_ANY 0xffff
#define DEVICE_ANY 0xffff
#define DEV_CLASS_DISPLAY 5
#define DRIVER_REGISTER(x)
#define GPU_CAP_SCANOUT 1
#define wmb() ((void)0)
static void host_log(const char *f,...){(void)f;}
#define kinfo host_log
#define kerr host_log
typedef struct {int held;} mutex_t;
static void mutex_init(mutex_t *m){m->held=0;}
static void mutex_lock(mutex_t *m){assert(!m->held);m->held=1;}
static void mutex_unlock(mutex_t *m){assert(m->held);m->held=0;}
static int mutex_trylock(mutex_t *m){if(m->held)return 0;m->held=1;return 1;}
typedef struct {uint64_t start,end;} resource_t;
typedef struct device {void *drv_priv;int managed,maps;int(*stop)(void*);void(*release)(void*);void *context;} device_t;
struct gpu_capabilities {uint64_t flags;};
typedef struct {int(*get_capabilities)(device_t*,struct gpu_capabilities*);int(*get_info)(device_t*,uint32_t*,uint32_t*,uint32_t*);int(*get_fb)(device_t*,uintptr_t*,size_t*);int(*flush)(device_t*,uint32_t,uint32_t,uint32_t,uint32_t);int(*ioctl)(device_t*,unsigned long,void*);int(*get_features)(device_t*,uint32_t*,uint32_t*);} gpu_dev_ops_t;
typedef struct {uint16_t vendor,device,subvendor,subdevice;} device_id_t;
typedef struct {const char *name;const device_id_t *id_table;void *bus;int(*probe)(device_t*);int(*remove)(device_t*);const gpu_dev_ops_t *class_ops;int class_type;} driver_t;
static int pci_bus;
static uint32_t readl(const volatile void *p);
static void writel(uint32_t v,volatile void *p);
static uint64_t va_to_pa(const void *p){return (uintptr_t)p;}
static void arch_dma_sync_for_device(void *p,size_t n);
static void arch_dma_sync_for_cpu(void *p,size_t n);
static void arch_cpu_relax(void);
static uint64_t clock_get_ticks(void);
static uint64_t clock_ticks_per_sec(void){return 1000;}
static int gpu_device_physical_mapping_init(device_t *d){d->managed=1;return 0;}
static int gpu_device_register(device_t *d);
static void gpu_device_unregister(device_t *d);
static int device_add_execution_action(device_t *d,int(*stop)(void*),void(*release)(void*),void *context);
static int pci_enable_and_assign_bars(device_t *d);
static resource_t *pci_get_bar_resource(device_t *d,unsigned n);
static uint32_t pci_device_id(device_t *d){assert(d);return 0x15ad0406;}
static void *device_map_resource(device_t *d,resource_t *r);
static uint16_t pci_cfg_read16(const device_t *d,uint32_t reg);
static void pci_cfg_write16(const device_t *d,uint32_t reg,uint16_t v);
/* PRODUCTION */
#define BASE 0x10000000UL
#define FBBASE 0x20000000UL
static device_t dev;
static resource_t resources[3];
static uint32_t registers[128];static uint16_t pci_command;
static uint8_t *framebuffer;
static uint64_t ticks;
static unsigned submits,stop_commands,unregisters,unmaps,polls;
static int fail_action,fail_pci,fail_map,fail_publish,bad_id,dead,hold,cancel_on_poll;
static int deny_mask,deny_enable,deny_config,deny_pci,fail_command_kind;
static uint32_t returned_status=1;
static int selected;
static uint64_t clock_get_ticks(void){return ticks++;}
static void arch_cpu_relax(void){
 polls++;assert(polls<100000);
 if(cancel_on_poll){cancel_on_poll=0;assert(dev.stop(dev.context)==-EBUSY);}
}
static void arch_dma_sync_for_device(void *p,size_t n){assert(p==g_vmsvga.command_page && n>64 && n<=PAGE_SIZE && g_vmsvga.lock.held);}
static void arch_dma_sync_for_cpu(void *p,size_t n){assert(p==g_vmsvga.command_page && n==64 && g_vmsvga.lock.held);}
static unsigned reg_offset(const volatile void *p){uintptr_t a=(uintptr_t)p;assert(a>=BASE && a<BASE+sizeof(registers) && dev.maps);return a-BASE;}
static uint32_t readl(const volatile void *p){
 unsigned reg=reg_offset(p);if(dead)return UINT32_MAX;
 if(reg==SVGA3_REG_ID && bad_id)return 0;
 return registers[reg/4];
}
static void writel(uint32_t v,volatile void *p){
 unsigned reg=reg_offset(p);assert(dev.managed && dev.stop);if(dead)return;
 if(reg==SVGA3_REG_IRQ_MASK && deny_mask)return;
 if(reg==SVGA3_REG_ENABLE && v==0 && deny_enable)return;
 if(reg==SVGA3_REG_CONFIG_DONE && v==0 && deny_config)return;
 registers[reg/4]=v;
 if(reg==SVGA3_REG_COMMAND_LO){
  svga3_command_buffer_t *h=(void*)g_vmsvga.command_page;
  assert(g_vmsvga.pending && g_vmsvga.lock.held && h->status==0 && h->flags==1);
  uint64_t pa=((uint64_t)registers[SVGA3_REG_COMMAND_HI/4]<<32)|(v&~0x3fU);
  assert(pa==(uintptr_t)h && h->physical_address==(uintptr_t)h+64);
  const uint32_t *cmd=(const void*)(g_vmsvga.command_page+64);submits++;
  int kind=2;
  if((v&0x3f)==SVGA3_CB_CONTEXT_DEVICE){
   assert(h->length==12 && cmd[0]==1 && cmd[2]==0);kind=cmd[1]?1:3;
   if(!cmd[1])stop_commands++;
  }else{assert(h->length==20 && (v&0x3f)==0);}
  if(!hold)h->status=fail_command_kind==kind?3:returned_status;
 }
}
static int device_add_execution_action(device_t *d,int(*stop)(void*),void(*release)(void*),void *context){
 assert(d->managed && d->drv_priv==context && !d->maps);
 if(fail_action)return -ENOMEM;
 d->stop=stop;d->release=release;d->context=context;return 0;
}
static int pci_enable_and_assign_bars(device_t *d){assert(d->stop);pci_command=7;return fail_pci?-EIO:0;}
static resource_t *pci_get_bar_resource(device_t *d,unsigned n){assert(d && n<3);return &resources[n];}
static void *device_map_resource(device_t *d,resource_t *r){
 if(fail_map==d->maps+1)return NULL;
 d->maps++;return r==&resources[0]?(void*)BASE:framebuffer;
}
static uint16_t pci_cfg_read16(const device_t *d,uint32_t reg){assert(d && reg==4);return dead?UINT16_MAX:pci_command;}
static void pci_cfg_write16(const device_t *d,uint32_t reg,uint16_t v){assert(d && reg==4);if(!dead && !deny_pci)pci_command=v;}
static int gpu_device_register(device_t *d){assert(d && !selected);if(fail_publish)return -EIO;selected=1;return 0;}
static void gpu_device_unregister(device_t *d){assert(d && selected && g_vmsvga.stopped && !g_vmsvga.pending);selected=0;unregisters++;}
static void reset(void){
 assert(!dev.context && !dev.drv_priv && !g_vmsvga.owner && !selected);
 memset(&dev,0,sizeof(dev));memset(registers,0,sizeof(registers));
 resources[0]=(resource_t){BASE,BASE+sizeof(registers)-1};resources[1]=resources[2]=(resource_t){FBBASE,FBBASE+4*1024*1024-1};
 registers[SVGA3_REG_BYTES_PER_LINE/4]=4096;registers[SVGA3_REG_FB_SIZE/4]=4*1024*1024;
 ticks=submits=stop_commands=unregisters=unmaps=polls=0;pci_command=0;
 fail_action=fail_pci=fail_map=fail_publish=bad_id=dead=hold=cancel_on_poll=0;
 deny_mask=deny_enable=deny_config=deny_pci=fail_command_kind=0;returned_status=1;
}
static int unbind(void){
 int ret=dev.stop?dev.stop(dev.context):0;if(ret)return ret;
 ret=vmsvga_driver.remove(&dev);if(ret)return ret;
 unmaps+=dev.maps;dev.maps=0;if(dev.release)dev.release(dev.context);
 dev.context=NULL;dev.stop=NULL;dev.release=NULL;return 0;
}
static void retained(void){assert(g_vmsvga.owner==&dev && dev.drv_priv==&g_vmsvga && dev.context && !unmaps);}
int main(void){
 framebuffer=calloc(1,4*1024*1024);assert(framebuffer);
 reset();fail_action=1;assert(vmsvga_driver.probe(&dev)==-ENOMEM && !g_vmsvga.owner);assert(!unbind());
 reset();fail_pci=1;assert(vmsvga_driver.probe(&dev)==-EIO);deny_pci=1;
 assert(unbind()==-EIO);retained();deny_pci=0;assert(!unbind());
 for(int stage=1;stage<=2;stage++){reset();fail_map=stage;assert(vmsvga_driver.probe(&dev)==-EIO);assert(!unbind());}
 reset();bad_id=1;assert(vmsvga_driver.probe(&dev)<0);assert(!unbind());
 reset();fail_publish=1;assert(vmsvga_driver.probe(&dev)<0 && g_vmsvga.context_live);assert(!unbind() && !selected);
 reset();assert(!vmsvga_driver.probe(&dev) && selected && submits==2);
 /* No spinlock/IRQ-off wait: task-context command mutex owns the page. */
 assert(!vmsvga_driver.class_ops->flush(&dev,0,0,1,1));
 hold=1;assert(vmsvga_driver.class_ops->flush(&dev,0,0,1,1)==-ETIMEDOUT);retained();
 uint8_t saved[PAGE_SIZE];memcpy(saved,g_vmsvga.command_page,PAGE_SIZE);unsigned count=submits;
 assert(vmsvga_driver.class_ops->flush(&dev,0,0,1,1)==-ESHUTDOWN);
 assert(unbind()==-ETIMEDOUT && submits==count && !memcmp(saved,g_vmsvga.command_page,PAGE_SIZE));retained();
 ((svga3_command_buffer_t*)g_vmsvga.command_page)->status=1;hold=0;assert(!unbind() && stop_commands==1 && unregisters==1);
 /* Stop may close an active call, but it cannot reuse/free that call's page. */
 reset();assert(!vmsvga_driver.probe(&dev));hold=1;cancel_on_poll=1;
 assert(vmsvga_driver.class_ops->flush(&dev,0,0,1,1)==-ESHUTDOWN && g_vmsvga.pending);retained();
 ((svga3_command_buffer_t*)g_vmsvga.command_page)->status=1;hold=0;assert(!unbind());
 reset();assert(!vmsvga_driver.probe(&dev));deny_mask=1;registers[SVGA3_REG_IRQ_MASK/4]=1;
 assert(unbind()==-EIO);retained();deny_mask=0;
 /* A timed-out STOP is still the sole page owner; completing it means a
    retry must not enqueue a second STOP before clearing PCI bus mastering. */
 hold=1;assert(unbind()==-ETIMEDOUT && g_vmsvga.pending_stop);count=stop_commands;retained();
 ((svga3_command_buffer_t*)g_vmsvga.command_page)->status=1;hold=0;deny_enable=1;
 assert(unbind()==-EIO && stop_commands==count);retained();deny_enable=0;
 deny_config=1;registers[SVGA3_REG_CONFIG_DONE/4]=1;assert(unbind()==-EIO);retained();deny_config=0;dead=1;
 assert(unbind()==-ENODEV);retained();dead=0;deny_pci=1;
 assert(unbind()==-EIO);retained();deny_pci=0;assert(!unbind());
 /* Unknown statuses (including partial completion/all ones) do not retire. */
 reset();assert(!vmsvga_driver.probe(&dev));returned_status=7;
 assert(vmsvga_driver.class_ops->flush(&dev,0,0,1,1)==-EIO && g_vmsvga.pending);
 assert(unbind()==-EIO);retained();((svga3_command_buffer_t*)g_vmsvga.command_page)->status=1;returned_status=1;assert(!unbind());
 reset();hold=1;assert(vmsvga_driver.probe(&dev)==-ETIMEDOUT && g_vmsvga.context_live);
 assert(unbind()==-ETIMEDOUT);retained();device_t other={0};assert(vmsvga_driver.probe(&other)==-EBUSY);
 ((svga3_command_buffer_t*)g_vmsvga.command_page)->status=1;hold=0;assert(!unbind());
 assert(SVGA2_REG_ENABLE==1 && SVGA2_REG_WIDTH==2 && SVGA2_REG_HEIGHT==3 && SVGA2_REG_FB_SIZE==16);
 free(framebuffer);puts("VMSVGA_LIFETIME_OK: no pending-page reuse; checked context/scanout/PCI stop and retained failure retry");
}

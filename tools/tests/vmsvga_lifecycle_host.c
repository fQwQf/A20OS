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
#define RES_MMIO 1
#define RES_IOPORT 2
#define CONFIG_IOPORT 1
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
typedef struct {uint64_t start,end;int type;} resource_t;
typedef struct device {void *drv_priv;int managed,maps,claims;int(*stop)(void*);void(*release)(void*);void *context;} device_t;
struct gpu_capabilities {uint64_t flags;};
typedef struct {uint32_t stride,red,green,blue,reserved;} gpu_scanout_layout_t;
typedef struct {int(*get_layout)(device_t*,gpu_scanout_layout_t*);int(*get_capabilities)(device_t*,struct gpu_capabilities*);int(*get_info)(device_t*,uint32_t*,uint32_t*,uint32_t*);int(*get_fb)(device_t*,uintptr_t*,size_t*);int(*flush)(device_t*,uint32_t,uint32_t,uint32_t,uint32_t);int(*ioctl)(device_t*,unsigned long,void*);int(*get_features)(device_t*,uint32_t*,uint32_t*);} gpu_dev_ops_t;
typedef struct {uint16_t vendor,device,subvendor,subdevice;} device_id_t;
typedef struct {const char *name;const device_id_t *id_table;void *bus;int(*probe)(device_t*);int(*remove)(device_t*);const gpu_dev_ops_t *class_ops;int class_type;} driver_t;
static int pci_bus;
static uint32_t readl(const volatile void *p);
static uint32_t inl(uint16_t p);
static void outl(uint16_t p,uint32_t v);
/* PORT_PRIMITIVES */
static int device_claim_resource(device_t *d,const resource_t *r);
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
static int legacy_mode;
static uint32_t pci_device_id(device_t *d){assert(d);return legacy_mode?0x15ad0405:0x15ad0406;}
static void *device_map_resource(device_t *d,resource_t *r);
static uint16_t pci_cfg_read16(const device_t *d,uint32_t reg);
static void pci_cfg_write16(const device_t *d,uint32_t reg,uint16_t v);
/* PRODUCTION */
#define BASE 0x10000000UL
#define FBBASE 0x20000000UL
static device_t dev;
static resource_t resources[3];
static uint32_t registers[128];static uint16_t pci_command;
static uint8_t *framebuffer,*fifo_memory;
static unsigned legacy_index,fifo_packets;
static uint32_t last_update[5];
#define IOBASE 0x4560U
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
static uint32_t legacy_get(unsigned index){
 assert(index<128);if(dead)return UINT32_MAX;
 if(index==0 && bad_id)return 0;
 return registers[index];
}
static void legacy_set(unsigned index,uint32_t v){
 assert(index<128);if(dead)return;
 if(index==1 && !v && deny_enable)return;
 if(index==20 && !v && deny_config)return;
 registers[index]=v;
 if(index==21){
  if(hold){registers[22]=1;return;}
  uint32_t *f=(void*)fifo_memory,min=f[0],max=f[1],next=f[2],stop=f[3];
  if((min|max|next|stop)&3 || min<16 || max>65536 || next>=max || stop>=max)return;
  while(stop!=next){
   for(unsigned i=0;i<5;i++){last_update[i]=f[stop/4];stop+=4;if(stop==max)stop=min;}
   assert(last_update[0]==1 && last_update[1]+last_update[3]<=1024 && last_update[2]+last_update[4]<=768);
   fifo_packets++;assert(fifo_packets<100);
  }
  f[3]=stop;registers[22]=0;
 }
}
static uint32_t inl(uint16_t p){assert(legacy_mode && p==IOBASE+1 && dev.claims==1);return legacy_get(legacy_index);}
static void outl(uint16_t p,uint32_t v){
 assert(legacy_mode && dev.claims==1);
 if(p==IOBASE)legacy_index=v;
 else{assert(p==IOBASE+1);legacy_set(legacy_index,v);}
}
static uint32_t readl(const volatile void *p){
 uintptr_t a=(uintptr_t)p;
 if(a>=(uintptr_t)fifo_memory && a<(uintptr_t)fifo_memory+65536){assert(dev.maps);return dead?UINT32_MAX:*(const uint32_t*)p;}
 if(legacy_mode){unsigned reg=reg_offset(p);assert(reg==4);return legacy_get(legacy_index);}

 unsigned reg=reg_offset(p);if(dead)return UINT32_MAX;
 if(reg==SVGA3_REG_ID && bad_id)return 0;
 return registers[reg/4];
}
static void writel(uint32_t v,volatile void *p){
 uintptr_t a=(uintptr_t)p;
 if(a>=(uintptr_t)fifo_memory && a<(uintptr_t)fifo_memory+65536){assert(dev.maps);if(!dead)*(uint32_t*)p=v;return;}
 if(legacy_mode){unsigned reg=reg_offset(p);if(!reg)legacy_index=v;else{assert(reg==4);legacy_set(legacy_index,v);}return;}

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
static int device_claim_resource(device_t *d,const resource_t *r){assert(d->stop && r==&resources[0] && r->type==RES_IOPORT);d->claims++;return 0;}
static int pci_enable_and_assign_bars(device_t *d){assert(d->stop);pci_command=7;return fail_pci?-EIO:0;}
static resource_t *pci_get_bar_resource(device_t *d,unsigned n){assert(d && n<3);return &resources[n];}
static void *device_map_resource(device_t *d,resource_t *r){
 if(fail_map==d->maps+1)return NULL;
 d->maps++;if(legacy_mode && r==&resources[2])return fifo_memory;
 return r==&resources[0]?(void*)BASE:framebuffer;
}
static uint16_t pci_cfg_read16(const device_t *d,uint32_t reg){assert(d && reg==4);return dead?UINT16_MAX:pci_command;}
static void pci_cfg_write16(const device_t *d,uint32_t reg,uint16_t v){assert(d && reg==4);if(!dead && !deny_pci)pci_command=v;}
static int gpu_device_register(device_t *d){assert(d && !selected);if(fail_publish)return -EIO;selected=1;return 0;}
static void gpu_device_unregister(device_t *d){assert(d && selected && g_vmsvga.stopped && !g_vmsvga.pending);selected=0;unregisters++;}
static void reset(void){
 assert(!dev.context && !dev.drv_priv && !g_vmsvga.owner && !selected);
 memset(&dev,0,sizeof(dev));memset(registers,0,sizeof(registers));
 resources[0]=(resource_t){BASE,BASE+sizeof(registers)-1,RES_MMIO};resources[1]=resources[2]=(resource_t){FBBASE,FBBASE+4*1024*1024-1,RES_MMIO};
 registers[SVGA3_REG_BYTES_PER_LINE/4]=4096;registers[SVGA3_REG_FB_SIZE/4]=4*1024*1024;
 ticks=submits=stop_commands=unregisters=unmaps=polls=0;pci_command=0;
 legacy_mode=legacy_index=fifo_packets=0;memset(fifo_memory,0,65536);
 fail_action=fail_pci=fail_map=fail_publish=bad_id=dead=hold=cancel_on_poll=0;
 deny_mask=deny_enable=deny_config=deny_pci=fail_command_kind=0;returned_status=1;
}
static int unbind(void){
 int ret=dev.stop?dev.stop(dev.context):0;if(ret)return ret;
 ret=vmsvga_driver.remove(&dev);if(ret)return ret;
 unmaps+=dev.maps;dev.maps=dev.claims=0;if(dev.release)dev.release(dev.context);
 dev.context=NULL;dev.stop=NULL;dev.release=NULL;return 0;
}
static void retained(void){assert(g_vmsvga.owner==&dev && dev.drv_priv==&g_vmsvga && dev.context && !unmaps);}
int main(void){
 framebuffer=calloc(1,4*1024*1024);fifo_memory=calloc(1,65536);assert(framebuffer && fifo_memory);
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
 /* Legacy BAR1 is scanout, BAR2 is FIFO; no SVGAv3 command is submitted. */
 for(unsigned ports=0;ports<=1;ports++){
  reset();legacy_mode=1;
  resources[0]=(resource_t){ports?IOBASE:BASE,(ports?IOBASE:BASE)+15,ports?RES_IOPORT:RES_MMIO};
  resources[2]=(resource_t){0x30000000,0x3000ffff,RES_MMIO};
  registers[12]=4160;registers[14]=128;registers[16]=4*1024*1024;registers[19]=65536;
  assert(!vmsvga_driver.probe(&dev) && selected && !submits && fifo_packets==1);
  uintptr_t fb;size_t size;gpu_scanout_layout_t layout;
  assert(!vmsvga_driver.class_ops->get_fb(&dev,&fb,&size) && fb==FBBASE+128 && size==4160*768);
  assert(!vmsvga_driver.class_ops->get_layout(&dev,&layout) && layout.stride==4160);
  assert(!vmsvga_driver.class_ops->flush(&dev,3,4,5,6) && fifo_packets==2);
  assert(last_update[1]==3 && last_update[2]==4 && last_update[3]==5 && last_update[4]==6);
  uint32_t *fifo=(void*)fifo_memory;fifo[2]=fifo[3]=65536-8;
  assert(!vmsvga_driver.class_ops->flush(&dev,0,0,1,1) && fifo[2]==16+12 && fifo[3]==fifo[2]);
  hold=1;assert(vmsvga_driver.class_ops->flush(&dev,0,0,1,1)==-ETIMEDOUT);retained();
  uint8_t saved_fifo[65536];memcpy(saved_fifo,fifo_memory,65536);
  assert(vmsvga_driver.class_ops->flush(&dev,1,1,1,1)==-ESHUTDOWN);
  assert(unbind()==-ETIMEDOUT && !memcmp(saved_fifo,fifo_memory,65536));retained();
  hold=0;assert(!unbind() && unmaps==(ports?2U:3U) && !dev.claims);
 }
 reset();legacy_mode=1;resources[0]=(resource_t){IOBASE,IOBASE+15,RES_IOPORT};resources[2]=(resource_t){0x30000000,0x3000ffff,RES_MMIO};
 registers[12]=4096;registers[16]=4*1024*1024;registers[19]=65536;
 assert(!vmsvga_driver.probe(&dev));uint32_t *f=(void*)fifo_memory;f[2]=65536;
 assert(vmsvga_driver.class_ops->flush(&dev,0,0,1,1)==-EIO && unbind()==-EIO);retained();
 f[2]=f[3]=16;dead=1;assert(unbind()==-ENODEV);retained();dead=0;assert(!unbind());
 free(fifo_memory);
 free(framebuffer);puts("VMSVGA_LIFETIME_OK: no pending-page reuse; checked context/scanout/PCI stop and retained failure retry");
}

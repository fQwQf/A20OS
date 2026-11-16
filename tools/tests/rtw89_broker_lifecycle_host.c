/* Production broker with modeled PCI, VFS and MM boundaries. No radio model. */
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include "kernel/include/uapi/a20/wifi.h"
#define PAGE_SIZE 4096
#define ROUND_UP(n,a) (((n)+(a)-1)&~((a)-1))
#define DMA_MASK_32BIT UINT32_MAX
#define PROT_READ 1
#define PROT_WRITE 2
#define MAP_SHARED 1
static void host_log(const char *format,...){(void)format;}
#define kinfo host_log
#define kwarn host_log
#define kerr host_log
typedef struct {int held;} mutex_t;
typedef struct {int held;} spinlock_t;
#define MUTEX_INIT {0}
static void mutex_init(mutex_t *p){p->held=0;}
static void mutex_lock(mutex_t *p){assert(!p->held);p->held=1;}
static void mutex_unlock(mutex_t *p){assert(p->held);p->held=0;}
static int mutex_trylock(mutex_t *p){if(p->held)return 0;p->held=1;return 1;}
static void spin_init(spinlock_t *p){p->held=0;}
static uint64_t spin_lock_irqsave(spinlock_t *p){assert(!p->held);p->held=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *p,uint64_t f){(void)f;assert(p->held);p->held=0;}
typedef struct {int refs;} mm_struct_t;
typedef struct {void *drv_priv;int managed;int(*execution)(void*);void *context;} device_t;
typedef struct {uint64_t start,end;} resource_t;
typedef struct vfile vfile_t;
typedef struct {
    int(*read)(vfile_t*,char*,size_t);
    int(*write)(vfile_t*,const char*,size_t);
    int(*ioctl)(vfile_t*,unsigned long,void*);
    int64_t(*mmap)(vfile_t*,uint64_t,size_t,int,int,uint64_t);
    int(*close)(vfile_t*);
} vfile_ops_t;
struct vfile {vfile_ops_t *ops;};
/* PRODUCTION_DEFINITIONS */
static struct {mm_struct_t *mm;} task;
static mm_struct_t owner={1};
static int capable=1,dead_config,stuck_disable,stuck_enable,fail_action,fail_mask;
static int assignments,config_writes,free_count,mm_drops,fail_unmap;
static unsigned char config[4096];
static resource_t bar={0x100000,0x17ffff};
static struct {mm_struct_t *mm;uint64_t address,physical;size_t length;} mappings[8];
static uint64_t next_va=0x10000000;
#define proc_current() (&task)
static void *userns_init_ns(void){return NULL;}
static int userns_capable(void *p,void *ns,int cap){(void)p;(void)ns;assert(cap==17);return capable;}
static int device_probe_use_remove(device_t *d){d->managed=1;return 0;}
static int device_add_execution_action(device_t *d,int(*fn)(void*),void(*release)(void*),void *p){
    assert(d->managed && d->drv_priv==&wifi && !release && !wifi.pci_touched);
    if(fail_action)return -ENOMEM;
    d->execution=fn;d->context=p;return 0;
}
static uint16_t pci_cfg_read16(device_t *d,unsigned offset){
    assert(d && offset<=sizeof(config)-2);
    if(dead_config)return UINT16_MAX;
    uint16_t v;memcpy(&v,config+offset,2);return v;
}
static void pci_cfg_write16(device_t *d,unsigned offset,uint16_t v){
    assert(d && d->managed && d->execution && offset<=sizeof(config)-2);
    config_writes++;
    if(offset==PCI_COMMAND){
        uint16_t old=pci_cfg_read16(d,offset);
        if(stuck_disable)v|=old&PCI_MASTER;
        if(stuck_enable)v&=~PCI_MASTER;
    }
    memcpy(config+offset,&v,2);
}
static uint32_t pci_cfg_read32(device_t *d,unsigned o){
    assert(d && o<=sizeof(config)-4);uint32_t v;memcpy(&v,config+o,4);return v;
}
static uint8_t pci_cfg_read8(device_t *d,unsigned o){assert(d && o<sizeof(config));return config[o];}
static void pci_cfg_write32(device_t *d,unsigned o,uint32_t v){assert(d && o<=sizeof(config)-4);memcpy(config+o,&v,4);}
static uint8_t pci_find_capability(device_t *d,unsigned c){assert(d && c==1);return 0;}
static int pci_enable_and_assign_bars(device_t *d){
    assert(!(pci_cfg_read16(d,PCI_COMMAND)&PCI_MASTER));assignments++;
    pci_cfg_write16(d,PCI_COMMAND,pci_cfg_read16(d,PCI_COMMAND)|6U);return 0;
}
static resource_t *pci_get_bar_resource(device_t *d,unsigned i){assert(d && i==2);return &bar;}
static int dma_set_mask(device_t *d,uint64_t mask){assert(d && mask==DMA_MASK_32BIT);return fail_mask?-EIO:0;}
static uint64_t dma_get_mask(device_t *d){assert(d);return DMA_MASK_32BIT;}
static void mdelay(unsigned ms){assert(ms==10);}
static long copy_to_user(void *d,const void *s,size_t n){if(!d)return -EFAULT;memcpy(d,s,n);return n;}
static long copy_from_user(void *d,const void *s,size_t n){if(!s)return -EFAULT;memcpy(d,s,n);return n;}
static int net_stack_device_hwaddr(device_t *d,const uint8_t *mac){assert(d && mac);return 0;}
static mm_struct_t *mm_get(mm_struct_t *mm){assert(mm->refs>0);mm->refs++;return mm;}
static void mm_destroy(mm_struct_t *mm){
    assert(!wifi_open_lock.held && !wifi.control.held && mm->refs>1);
    for(unsigned i=0;i<8;i++)assert(mappings[i].mm!=mm);
    mm->refs--;mm_drops++;
}
static int64_t wifi_map(mm_struct_t *mm,uint64_t physical,size_t length,int is_device){
    (void)is_device;assert(mm && length && wifi.control.held);
    for(unsigned i=0;i<8;i++)if(!mappings[i].mm){
        mappings[i].mm=mm;mappings[i].physical=physical;mappings[i].length=length;
        mappings[i].address=next_va;next_va+=length;return mappings[i].address;
    }
    return -ENOMEM;
}
static int mm_munmap(mm_struct_t *mm,uint64_t address,size_t length){
    assert(mm && wifi.control.held);
    if(fail_unmap)return -ENOMEM;
    for(unsigned i=0;i<8;i++)if(mappings[i].address==address && mappings[i].mm==mm){
        assert(mappings[i].length==length);memset(&mappings[i],0,sizeof(mappings[i]));return 0;
    }
    assert(!"untracked unmap");return -EINVAL;
}
static uint64_t va_to_pa(void *p){return (uintptr_t)p;}
static void *dma_alloc_coherent_aligned(device_t *d,size_t n,size_t align,uint64_t *bus){
    assert(d && wifi.control.held);void *p=aligned_alloc(align,n);assert(p);*bus=(uintptr_t)p;return p;
}
static void dma_free_coherent_aligned(void *p,size_t n,uint64_t bus){
    assert(p && n && bus==(uintptr_t)p && wifi.control.held);
    for(unsigned i=0;i<8;i++)assert(!mappings[i].mm || mappings[i].physical!=bus);
    free(p);free_count++;
}
/* PRODUCTION_FUNCTIONS */
static device_t dev;
static vfile_t file;
static void fresh(void){
    assert(!wifi.device && !wifi.owner && !wifi.dma_bytes && owner.refs==1);
    memset(&dev,0,sizeof(dev));memset(config,0,sizeof(config));
    uint16_t command=6;memcpy(config+PCI_COMMAND,&command,2);
    task.mm=&owner;assert(!wifi_probe(&dev));
    assert(dev.managed && dev.execution==wifi_stop && dev.context==&dev);
    assert(wifi.device==&dev && dev.drv_priv==&wifi && !wifi.closing);
}
static struct a20_wifi_dma mapped_dma(void){
    struct a20_wifi_dma dma={.size=4096};
    assert(!wifi_ioctl(&file,A20_WIFI_DMA_ALLOC,&dma));
    assert(wifi_mmap(&file,0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,
                     dma.token*A20_WIFI_DMA_OFFSET)>0);
    return dma;
}
int main(void){
    fresh();int probes=assignments;
    capable=0;assert(rtw89_file_open(&file)==-EPERM && owner.refs==1);capable=1;
    stuck_enable=1;assert(rtw89_file_open(&file)==-EIO && !wifi.owner && owner.refs==1);
    stuck_enable=0;assert(!rtw89_file_open(&file) && owner.refs==2 && file.ops==&wifi_file_ops);
    assert(rtw89_file_open(&file)==-EBUSY);
    struct a20_wifi_dma dma=mapped_dma();
    assert(wifi_mmap(&file,0,wifi.bar_size,PROT_READ|PROT_WRITE,MAP_SHARED,0)>0);
    device_t other={0};assert(wifi_probe(&other)==-EBUSY && assignments==probes);
    wifi.carrier=1;mutex_lock(&wifi_open_lock);
    assert(dev.execution(dev.context)==-EBUSY && wifi.closing && !wifi.carrier);
    mutex_unlock(&wifi_open_lock);mutex_lock(&wifi.control);
    assert(dev.execution(dev.context)==-EBUSY);mutex_unlock(&wifi.control);
    assert(wifi_remove(&dev)==-EBUSY && dev.drv_priv==&wifi && wifi.device==&dev);
    assert(!(pci_cfg_read16(&dev,PCI_COMMAND)&PCI_MASTER));
    assert(wifi_ioctl(&file,A20_WIFI_INFO,NULL)==-ENODEV);
    assert(wifi_ioctl(&file,A20_WIFI_CFG_WRITE,NULL)==-ENODEV);
    assert(wifi_ioctl(&file,A20_WIFI_DMA_ALLOC,NULL)==-ENODEV);
    assert(wifi_ioctl(&file,A20_WIFI_LINK,NULL)==-ENODEV);
    assert(wifi_mmap(&file,0,4096,PROT_READ|PROT_WRITE,MAP_SHARED,0)==-ENODEV);
    char packet[64]={0};assert(wifi_send(&dev,packet,64)==-ENETDOWN);
    assert(wifi_recv(&dev,packet,64)==-ENODEV && wifi_read(&file,packet,64)==-ENODEV);
    assert(wifi_write(&file,packet,64)==-ENETDOWN);
    uint16_t command=PCI_MASTER;memcpy(config+PCI_COMMAND,&command,2);stuck_disable=1;
    assert(wifi_ioctl(&file,A20_WIFI_DMA_FREE,&dma)==-EIO && !free_count);
    assert(wifi_remove(&dev)==-EIO && wifi.dma[0].mapped && dev.drv_priv==&wifi);
    stuck_disable=0;fail_unmap=1;
    assert(wifi_ioctl(&file,A20_WIFI_DMA_FREE,&dma)==-ENOMEM);
    assert(wifi.dma[0].mapped && wifi.dma[0].memory && wifi.dma_bytes==4096 && !free_count);
    fail_unmap=0;assert(!wifi_ioctl(&file,A20_WIFI_DMA_FREE,&dma) && free_count==1);
    assert(!wifi_ioctl(&file,A20_WIFI_QUIESCE,NULL));
    assert(!wifi_close(&file) && !wifi.owner && owner.refs==1 && mm_drops==1);
    assert(!wifi_remove(&dev) && !dev.drv_priv && wifi_transport_available());
    assert(!wifi_remove(&dev) && !wifi_remove(NULL) && free_count==1);

    /* A failed close keeps its BAR mapping and mm ref for a stop retry. */
    fresh();assert(!rtw89_file_open(&file));
    assert(wifi_mmap(&file,0,wifi.bar_size,PROT_READ|PROT_WRITE,MAP_SHARED,0)>0);
    fail_unmap=1;assert(wifi_close(&file)==-ENOMEM);
    assert(wifi.owner==&owner && wifi.owner_closed && wifi.mmio_va && owner.refs==2);
    assert(wifi_remove(&dev)==-ENOMEM && dev.drv_priv==&wifi);
    fail_unmap=0;dead_config=1;
    assert(wifi_remove(&dev)==-ENODEV && owner.refs==2 && wifi.mmio_va);
    dead_config=0;assert(!wifi_remove(&dev) && owner.refs==1 && wifi_transport_available());

    /* Managed partial probes never clear a binding if BME stop fails. */
    memset(&dev,0,sizeof(dev));fail_action=1;int writes=config_writes;
    assert(wifi_probe(&dev)==-ENOMEM && config_writes==writes);
    assert(!wifi_remove(&dev) && wifi_transport_available());fail_action=0;
    memset(&dev,0,sizeof(dev));command=6;memcpy(config+PCI_COMMAND,&command,2);
    stuck_disable=1;probes=assignments;
    assert(wifi_probe(&dev)==-EIO && dev.managed && dev.drv_priv==&wifi);
    assert(assignments==probes && wifi_remove(&dev)==-EIO);
    assert(wifi_probe(&other)==-EBUSY);stuck_disable=0;
    assert(!wifi_remove(&dev) && wifi_transport_available());
    memset(&dev,0,sizeof(dev));fail_mask=1;
    assert(wifi_probe(&dev)==-EOPNOTSUPP && dev.drv_priv==&wifi);
    assert(!wifi_remove(&dev));fail_mask=0;

    /* Abnormal service exit has no radio idle receipt: never free its DMA. */
    fresh();assert(!rtw89_file_open(&file));dma=mapped_dma();
    assert(wifi_mmap(&file,0,wifi.bar_size,PROT_READ|PROT_WRITE,MAP_SHARED,0)>0);
    fail_unmap=1;assert(wifi_close(&file)==-ENOMEM && wifi.quarantined);
    assert(wifi.owner_closed && owner.refs==2 && wifi.dma[0].mapped && wifi.mmio_va);
    assert(wifi_remove(&dev)==-ENOMEM && wifi.device==&dev && free_count==1);
    fail_unmap=0;assert(wifi_remove(&dev)==-EBUSY);
    assert(!wifi.owner && owner.refs==1 && !wifi.mmio_va && !wifi.dma[0].mapped);
    assert(wifi.device==&dev && dev.drv_priv==&wifi && wifi.quarantined && wifi.dma_bytes==4096);
    assert(!wifi_transport_available() && wifi_probe(&other)==-EBUSY);
    assert(rtw89_file_open(&file)==-ENODEV && free_count==1);
    /* Harness process teardown only; production keeps this until reboot. */
    free(wifi.dma[0].memory);
    return 0;
}

/* Actual GPU queue/initialization/teardown and transport reset. Register,
 * clock, IRQ, lifecycle admission and allocation/aliases are host models. */
#include "drivers/gpu/virtio_gpu.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/bus/virtio_transport.h"
#include "drivers/block/virtio_blk.h"
#define ALIGNED(n) __attribute__((aligned(n)))
#define VIRTIO_GPU_QUEUE_SIZE VIRTIO_QUEUE_SIZE
#define VIRTIO_GPU_COMMAND_BYTES 128U
#define mb() __sync_synchronize()
#define wmb() __sync_synchronize()
#define kinfo(...) ((void)0)
#define MS_TO_TICKS(ms) ((uint64_t)(ms)*hz/1000)
#define va_to_pa(p) ((uint64_t)(uintptr_t)(p))
typedef pthread_mutex_t mutex_t;
static void mutex_init(mutex_t *m){pthread_mutexattr_t a;assert(!pthread_mutexattr_init(&a));assert(!pthread_mutexattr_settype(&a,PTHREAD_MUTEX_RECURSIVE));assert(!pthread_mutex_init(m,&a));assert(!pthread_mutexattr_destroy(&a));}
static void mutex_lock(mutex_t *m){assert(!pthread_mutex_lock(m));}
static int mutex_trylock(mutex_t *m){return !pthread_mutex_trylock(m);}
static void mutex_unlock(mutex_t *m){assert(!pthread_mutex_unlock(m));}
static uint64_t now=10,hz=1000,step;
static unsigned relaxes,yields;
static uint64_t clock_get_ticks(void){uint64_t t=now;now+=step;return t;}
static uint64_t clock_ticks_per_sec(void){return hz;}
static void arch_cpu_relax(void){relaxes++;}
static int task_present;
static void *proc_current(void){return task_present?(void *)1:NULL;}
static void proc_yield(void){yields++;}
static void arch_dma_sync_for_device(const void *p,size_t n){(void)p;(void)n;}
static void arch_dma_sync_for_cpu(const void *p,size_t n){(void)p;(void)n;}
typedef unsigned proc_wait_token_t;
typedef struct {unsigned unused;} wait_queue_entry_t;
#define PROC_WAIT_UNINTERRUPTIBLE 0
static proc_wait_token_t proc_park_prepare(int mode,uint64_t until){(void)mode;(void)until;return 1;}
static void wait_queue_link(wait_queue_t *q,wait_queue_entry_t *e,proc_wait_token_t t,int unused){(void)q;(void)e;(void)t;(void)unused;}
static void wait_queue_unlink(wait_queue_t *q,wait_queue_entry_t *e){(void)q;(void)e;}
static int proc_park_cancel(proc_wait_token_t t){(void)t;return 0;}
static int proc_park_commit(proc_wait_token_t t){(void)t;return 0;}
static void proc_park_finish(proc_wait_token_t t){(void)t;}
/* PRODUCTION_INSTANCE */
/* PRODUCTION_CLAIM */
struct hardware {unsigned resets,reads,delay,never,notify,bad_response_at,no_complete,bad_id,bad_advance,feature_select,status,irq_get,irq_put;int pci_error;};
static struct hardware hw;
static device_t *selected;
static int opted,strict_probe;
static uint32_t transport_read(virtio_transport_t *t,uint32_t off){
 assert(t->priv==&hw);if(strict_probe)assert(opted);
 switch(off){
 case VIRTIO_MMIO_MAGIC:return 0x74726976;
 case VIRTIO_MMIO_VERSION:return 2;
 case VIRTIO_MMIO_DEVICE_ID:return 16;
 case VIRTIO_MMIO_DEVICE_FEATURES:return hw.feature_select?VIRTIO_F_VERSION_1_BIT:0;
 case VIRTIO_MMIO_QUEUE_NUM_MAX:return VIRTIO_QUEUE_SIZE;
 case VIRTIO_MMIO_STATUS:hw.reads++;if(hw.resets&&!hw.status){if(hw.never||hw.reads<=hw.delay)return 4;return 0;}return hw.status;
 default:return 0;
 }
}
static void transport_write(virtio_transport_t *t,uint32_t off,uint32_t value){
 assert(t->priv==&hw);if(strict_probe)assert(opted);
 if(off==VIRTIO_MMIO_STATUS){hw.status=value;if(!value){hw.resets++;hw.reads=0;}return;}
 if(off==VIRTIO_MMIO_DEVICE_FEATURES_SEL){hw.feature_select=value;return;}
 if(off!=VIRTIO_MMIO_QUEUE_NOTIFY)return;
 hw.notify++;if(hw.no_complete)return;
 virtio_gpu_inst_t *inst=&g_gpu_inst;uint16_t head=inst->avail.ring[(inst->avail.idx-1)%VIRTIO_GPU_QUEUE_SIZE],last=head;
 while(inst->desc[last].flags&VIRTQ_DESC_F_NEXT)last=inst->desc[last].next;
 struct virtio_gpu_ctrl_hdr *req=(void *)(uintptr_t)inst->desc[head].addr,*resp=(void *)(uintptr_t)inst->desc[last].addr;
 memset(resp,0,inst->desc[last].len);resp->type=hw.notify==hw.bad_response_at?0:VIRTIO_GPU_RESP_OK_NODATA;resp->fence_id=req->fence_id;
 if(req->type==VIRTIO_GPU_CMD_GET_DISPLAY_INFO){struct virtio_gpu_resp_display_info *d=(void *)resp;d->hdr.type=VIRTIO_GPU_RESP_OK_DISPLAY_INFO;d->width=64;d->height=32;}
 inst->used.ring[inst->used.idx%VIRTIO_GPU_QUEUE_SIZE].id=head+hw.bad_id;inst->used.idx+=(hw.bad_advance?hw.bad_advance:1);
}
/* PRODUCTION_RESET */
static int request_irq(uint32_t irq,int (*handler)(int,void *),unsigned long flags,void *arg){(void)handler;(void)flags;assert(irq==5&&arg==&g_gpu_inst&&opted);hw.irq_get++;return 0;}
static void free_irq(uint32_t irq,void *arg){assert(irq==5&&arg==&g_gpu_inst&&!hw.status&&g_gpu_inst.reset_confirmed&&!g_gpu_inst.command_active);hw.irq_put++;}
#define IRQF_SHARED 1
static int virtio_gpu_irq_handler(int irq,void *arg){(void)irq;(void)arg;return 0;}
int gpu_device_register(device_t *d){assert(d->drv_priv==&g_gpu_inst);selected=d;return 0;}
void gpu_device_unregister(device_t *d){if(selected==d)selected=NULL;}
int device_probe_use_remove(device_t *d){assert(d);opted=1;return 0;}
static bus_type_t pci_bus;
static int pci_virtio_transport_init(device_t *d,unsigned type,virtio_transport_t *t){assert(d&&type==16&&opted);if(hw.pci_error)return hw.pci_error;*t=(virtio_transport_t){.priv=&hw,.read32=transport_read,.write32=transport_write,.irq=5};return 0;}
static void virtio_gpu_mmio_write32(virtio_transport_t *t,uint32_t o,uint32_t v){transport_write(t,o,v);}
static uint32_t virtio_gpu_mmio_read32(virtio_transport_t *t,uint32_t o){return transport_read(t,o);}
resource_t *device_get_resource(device_t *d,enum resource_type type,int index){(void)d;(void)type;(void)index;abort();}
void *device_map_mmio(device_t *d,int index){(void)d;(void)index;abort();}
static void virtio_gpu_fetch_edid(virtio_gpu_inst_t *inst){assert(!inst->has_edid);}
static int virtio_gpu_get_capset_info(virtio_gpu_inst_t *inst,uint32_t i,uint32_t *id,uint32_t *version,uint32_t *size){(void)inst;(void)i;(void)id;(void)version;(void)size;return -EOPNOTSUPP;}
/* PRODUCTION_COMMANDS */
/* PRODUCTION_LIFETIME */
static int issue(unsigned path){
 struct virtio_gpu_ctrl_hdr req={.type=VIRTIO_GPU_CMD_RESOURCE_FLUSH},resp={0};unsigned char bytes[256]={0};struct virtio_gpu_mem_entry entry={.addr=4096,.length=4096};
 switch(path){case 0:return virtio_gpu_send_cmd(&g_gpu_inst,&req,sizeof(req),&resp,sizeof(resp));case 1:return virtio_gpu_send_cmd_big(&g_gpu_inst,bytes,sizeof(bytes),&resp,sizeof(resp));case 2:return virtio_gpu_submit_3d(&g_gpu_inst,1,bytes,sizeof(bytes));default:return virtio_gpu_resource_attach_backing(&g_gpu_inst,1,&entry,1);}
}
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];size_t capacity=(size_t)1<<MAX_ORDER;
 unsigned char *ram=aligned_alloc(PAGE_SIZE,capacity*PAGE_SIZE);assert(ram);host_page_offset=(uintptr_t)ram;pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0,.end=capacity*PAGE_SIZE,.start_pfn=0,.end_pfn=capacity};pfa.total_frames=capacity;pfa.meta=calloc(capacity,sizeof(*pfa.meta));assert(pfa.meta);spin_init(&pfa.lock);
 device_t dev={.bus=&pci_bus},other={.bus=&pci_bus};virtio_transport_t vt={.priv=&hw,.read32=transport_read,.write32=transport_write};
 if(!strncmp(name,"reset-",6)){
  if(!strcmp(name,"reset-parameters")){assert(virtio_transport_reset(NULL,1)==-EINVAL);assert(virtio_transport_reset(&vt,0)==-EINVAL);assert(virtio_transport_reset(&vt,1001)==-EINVAL);vt.read32=NULL;assert(virtio_transport_reset(&vt,1)==-EINVAL);vt.read32=transport_read;vt.write32=NULL;assert(virtio_transport_reset(&vt,1)==-EINVAL);assert(!hw.resets&&!hw.reads);}
  else if(!strcmp(name,"reset-delayed")){hw.delay=7;assert(!virtio_transport_reset(&vt,100)&&hw.resets==1&&hw.reads==8);}
  else if(!strcmp(name,"reset-deadline")){hw.never=1;step=50;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==2);hw.never=0;assert(!virtio_transport_reset(&vt,100)&&hw.resets==2);}
  else if(!strcmp(name,"reset-stalled")){hw.never=1;task_present=1;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==1000000&&yields);hz=0;task_present=0;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==1000000);}
  else {assert(!strcmp(name,"reset-wrap"));now=((uint64_t)-1)-50;step=50;hw.never=1;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==2);hz=((uint64_t)-1);step=0;hw.never=0;hw.delay=3;assert(!virtio_transport_reset(&vt,1000));}
 }else{
  strict_probe=1;
  if(!strcmp(name,"probe-pci-fail")){hw.pci_error=-ENOMEM;assert(virtio_gpu_probe(&dev)==-ENOMEM&&dev.drv_priv&&opted&&!hw.resets);virtio_gpu_quiesce(&dev);assert(!virtio_gpu_remove(&dev)&&!dev.drv_priv&&!g_gpu_owner);}
  else if(!strcmp(name,"probe-vmo-oom")){fail_at=2;assert(virtio_gpu_probe(&dev)<0&&dev.drv_priv&&!g_gpu_inst.fb_memory&&hw.irq_get==1&&!hw.irq_put);virtio_gpu_quiesce(&dev);hw.never=1;step=50;assert(virtio_gpu_remove(&dev)==-ETIMEDOUT&&!hw.irq_put&&dev.drv_priv);hw.never=0;assert(!virtio_gpu_remove(&dev)&&hw.irq_put==1&&!dev.drv_priv&&!g_gpu_owner);}
  else if(!strcmp(name,"probe-reset-fail")){hw.never=1;step=50;assert(virtio_gpu_probe(&dev)==-ETIMEDOUT&&dev.drv_priv&&g_gpu_owner==&dev&&!hw.notify);assert(virtio_gpu_probe(&other)==-EBUSY&&!other.drv_priv&&g_gpu_owner==&dev);virtio_gpu_quiesce(&dev);assert(virtio_gpu_remove(&dev)==-ETIMEDOUT&&dev.drv_priv&&g_gpu_owner==&dev&&!hw.irq_put);hw.never=0;assert(!virtio_gpu_remove(&dev)&&!dev.drv_priv&&!g_gpu_owner);}
  else{
   if(!strcmp(name,"probe-partial"))hw.bad_response_at=3;
   int probed=virtio_gpu_probe(&dev);assert((!strcmp(name,"probe-partial")?probed<0:probed==0)&&dev.drv_priv&&g_gpu_owner==&dev&&g_gpu_inst.fb_memory&&hw.irq_get==1);
   struct vmo *snapshot=g_gpu_inst.fb_memory;vmo_ref(snapshot);pfn_t page=vmo_peek_page(snapshot,0);((unsigned char *)pfn_to_virt(page))[0]=0x7a;size_t pages=allocated_pages;
   assert(virtio_gpu_probe(&other)==-EBUSY&&!other.drv_priv&&g_gpu_inst.fb_memory==snapshot);
   if(!strncmp(name,"command-",8)){
    unsigned path=(unsigned)(name[strlen(name)-1]-'0');g_gpu_inst.virgl=1;
    if(strstr(name,"oom")){unsigned notify=hw.notify;uint16_t idx=g_gpu_inst.avail.idx;fail_at=attempts+1;assert(issue(path)==-ENOMEM&&!g_gpu_inst.command_failed&&!g_gpu_inst.command_active&&hw.notify==notify&&g_gpu_inst.avail.idx==idx);fail_at=0;assert(!issue(path));}
    else if(strstr(name,"timeout")){hw.no_complete=1;step=1000;assert(issue(path)==-ETIMEDOUT&&g_gpu_inst.command_failed);}
    else if(strstr(name,"wrong-id")){hw.bad_id=7;assert(issue(path)==-EIO&&g_gpu_inst.command_failed);}
    else if(strstr(name,"wrong-index")){hw.bad_advance=2;assert(issue(path)==-EIO&&g_gpu_inst.command_failed);}
    else{assert(strstr(name,"success"));assert(!issue(path)&&!g_gpu_inst.command_failed);}
    if(g_gpu_inst.command_failed){unsigned notify=hw.notify;size_t allocs=attempts;uint16_t idx=g_gpu_inst.avail.idx;for(unsigned j=0;j<4;j++)assert(issue(j)==-EIO);assert(hw.notify==notify&&attempts==allocs&&g_gpu_inst.avail.idx==idx&&!g_gpu_inst.command_active);}
   }else if(!strcmp(name,"remove-active")){assert(!virtio_gpu_command_begin(&g_gpu_inst));virtio_gpu_quiesce(&dev);assert(virtio_gpu_remove(&dev)==-EBUSY&&hw.resets==1&&!hw.irq_put&&g_gpu_inst.fb_memory==snapshot);virtio_gpu_command_end(&g_gpu_inst);}
   else assert(!strcmp(name,"remove-retained")||!strcmp(name,"probe-partial"));
   virtio_gpu_quiesce(&dev);unsigned notify=hw.notify;g_gpu_inst.virgl=1;for(unsigned j=0;j<4;j++)assert(issue(j)==-ENODEV);assert(hw.notify==notify);
   hw.never=1;step=50;assert(virtio_gpu_remove(&dev)==-ETIMEDOUT&&dev.drv_priv&&g_gpu_owner==&dev&&g_gpu_inst.fb_memory==snapshot&&!hw.irq_put&&allocated_pages==pages);
   hw.never=0;assert(!virtio_gpu_remove(&dev)&&!dev.drv_priv&&!g_gpu_owner&&!selected&&hw.irq_put==1&&allocated_pages==pages);
   assert(((unsigned char *)pfn_to_virt(page))[0]==0x7a);vmo_release(snapshot);assert(!allocated_pages);assert(!virtio_gpu_remove(&dev)&&hw.irq_put==1);
  }
  assert(!pthread_mutex_destroy(&g_gpu_inst.command_lock));
 }
 assert(!heap_live&&!allocated_pages&&!g_a20_objstats.vmos&&!g_a20_objstats.vmo_pages);free(pfa.meta);free(ram);return 0;
}

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
#define DMA_MASK_64BIT ((uint64_t)-1)
static int architecture_ready=1;
int arch_device_dma_ready(void) __attribute__((weak));
bool device_is_registered(const device_t *d){return d&&d->references!=0;}
device_t *device_get(device_t *d){if(d&&d->references){d->references++;return d;}return NULL;}
void device_put(device_t *d){assert(d&&d->references>1);d->references--;}
pfn_t pfa_alloc_max(int order,uint64_t mask){pfn_t p=pfa_alloc(order);if(p!=PFN_NONE&&pfn_to_phys(p)+((size_t)PAGE_SIZE<<order)-1>mask){pfa_free(p,order);return PFN_NONE;}return p;}
static void instance_allocation_free(pfn_t,int);
#define pfa_free instance_allocation_free
/* PRODUCTION_BUFFER */
#undef pfa_free
int arch_device_dma_ready(void){return architecture_ready;}
/* PRODUCTION_INSTANCE */
/* PRODUCTION_CLAIM */
struct hardware {unsigned resets,reads,delay,never,notify,bad_response_at,no_complete,bad_id,bad_advance,feature_select,status,irq_get,irq_put,width,height;int pci_error,opted;virtio_gpu_inst_t *instance;};
static struct hardware hw,peer;
static device_t *selected;
static int opted,strict_probe;
static uint32_t transport_read(virtio_transport_t *t,uint32_t off){
 struct hardware *h=t->priv;assert(h==&hw||h==&peer);if(strict_probe)assert(h->opted);
 switch(off){
 case VIRTIO_MMIO_MAGIC:return 0x74726976;
 case VIRTIO_MMIO_VERSION:return 2;
 case VIRTIO_MMIO_DEVICE_ID:return 16;
 case VIRTIO_MMIO_DEVICE_FEATURES:return h->feature_select?VIRTIO_F_VERSION_1_BIT:0;
 case VIRTIO_MMIO_QUEUE_NUM_MAX:return VIRTIO_QUEUE_SIZE;
 case VIRTIO_MMIO_STATUS:h->reads++;if(h->resets&&!h->status){if(h->never||h->reads<=h->delay)return 4;return 0;}return h->status;
 default:return 0;
 }
}
static void transport_write(virtio_transport_t *t,uint32_t off,uint32_t value){
 struct hardware *h=t->priv;assert(h==&hw||h==&peer);if(strict_probe)assert(h->opted);
 if(off==VIRTIO_MMIO_STATUS){h->status=value;if(!value){h->resets++;h->reads=0;}return;}
 if(off==VIRTIO_MMIO_DEVICE_FEATURES_SEL){h->feature_select=value;return;}
 if(off!=VIRTIO_MMIO_QUEUE_NOTIFY)return;
 h->notify++;if(h->no_complete)return;
 virtio_gpu_inst_t *inst=h->instance;assert(inst&&inst->vt.priv==h);uint16_t head=inst->avail.ring[(inst->avail.idx-1)%VIRTIO_GPU_QUEUE_SIZE],last=head;
 while(inst->desc[last].flags&VIRTQ_DESC_F_NEXT)last=inst->desc[last].next;
 struct virtio_gpu_ctrl_hdr *req=(void *)(uintptr_t)inst->desc[head].addr,*resp=(void *)(uintptr_t)inst->desc[last].addr;
 memset(resp,0,inst->desc[last].len);resp->type=h->notify==h->bad_response_at?0:VIRTIO_GPU_RESP_OK_NODATA;resp->fence_id=req->fence_id;
 if(req->type==VIRTIO_GPU_CMD_GET_DISPLAY_INFO){struct virtio_gpu_resp_display_info *d=(void *)resp;d->hdr.type=VIRTIO_GPU_RESP_OK_DISPLAY_INFO;d->width=h->width?h->width:64;d->height=h->height?h->height:32;}
 inst->used.ring[inst->used.idx%VIRTIO_GPU_QUEUE_SIZE].id=head+h->bad_id;inst->used.idx+=(h->bad_advance?h->bad_advance:1);
}
static void instance_allocation_free(pfn_t p,int order){
 struct hardware *list[]={&hw,&peer};
 for(unsigned i=0;i<2;i++){struct hardware *h=list[i];virtio_gpu_inst_t *inst=h->instance;
  if(!inst||pfn_to_virt(p)!=(void *)inst)continue;
  assert(!inst->transport_live||(inst->reset_confirmed&&!h->status));assert(h->irq_get==h->irq_put&&!inst->command_active);
  assert(!pthread_mutex_destroy(&inst->command_lock));h->instance=NULL;
 }
 pfa_free(p,order);
}
/* PRODUCTION_RESET */
static int request_irq(uint32_t irq,int (*handler)(int,void *),unsigned long flags,void *arg){(void)handler;(void)flags;virtio_gpu_inst_t *inst=arg;struct hardware *h=inst->vt.priv;assert(irq==5&&h->instance==inst&&h->opted);h->irq_get++;return 0;}
static void free_irq(uint32_t irq,void *arg){virtio_gpu_inst_t *inst=arg;struct hardware *h=inst->vt.priv;assert(irq==5&&h->instance==inst&&!h->status&&inst->reset_confirmed&&!inst->command_active);h->irq_put++;}
#define IRQF_SHARED 1
static int virtio_gpu_irq_handler(int irq,void *arg){(void)irq;(void)arg;return 0;}
int gpu_device_register(device_t *d){assert(d->drv_priv);if(!selected)selected=d;return 0;}
void gpu_device_unregister(device_t *d){if(selected==d)selected=NULL;}
int device_probe_use_remove(device_t *d){assert(d&&d->plat_data);((struct hardware *)d->plat_data)->opted=1;opted=1;return 0;}
static bus_type_t pci_bus;
static int pci_virtio_transport_init(device_t *d,unsigned type,virtio_transport_t *t){struct hardware *h=d->plat_data;assert(d&&type==16&&h->opted);h->instance=d->drv_priv;if(h->pci_error)return h->pci_error;*t=(virtio_transport_t){.priv=h,.read32=transport_read,.write32=transport_write,.irq=5};return 0;}
static void virtio_gpu_mmio_write32(virtio_transport_t *t,uint32_t o,uint32_t v){transport_write(t,o,v);}
static uint32_t virtio_gpu_mmio_read32(virtio_transport_t *t,uint32_t o){return transport_read(t,o);}
resource_t *device_get_resource(device_t *d,enum resource_type type,int index){(void)d;(void)type;(void)index;abort();}
void *device_map_mmio(device_t *d,int index){(void)d;(void)index;abort();}
static void virtio_gpu_fetch_edid(virtio_gpu_inst_t *inst){assert(!inst->has_edid);}
static int virtio_gpu_get_capset_info(virtio_gpu_inst_t *inst,uint32_t i,uint32_t *id,uint32_t *version,uint32_t *size){(void)inst;(void)i;(void)id;(void)version;(void)size;return -EOPNOTSUPP;}
/* PRODUCTION_COMMANDS */
/* PRODUCTION_LIFETIME */
static int issue(virtio_gpu_inst_t *inst,unsigned path){
 struct virtio_gpu_ctrl_hdr req={.type=VIRTIO_GPU_CMD_RESOURCE_FLUSH},resp={0};unsigned char bytes[256]={0};struct virtio_gpu_mem_entry entry={.addr=4096,.length=4096};
 switch(path){case 0:return virtio_gpu_send_cmd(inst,&req,sizeof(req),&resp,sizeof(resp));case 1:return virtio_gpu_send_cmd_big(inst,bytes,sizeof(bytes),&resp,sizeof(resp));case 2:return virtio_gpu_submit_3d(inst,1,bytes,sizeof(bytes));default:return virtio_gpu_resource_attach_backing(inst,1,&entry,1);}
}
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];size_t capacity=(size_t)1<<MAX_ORDER;
 unsigned char *ram=aligned_alloc(PAGE_SIZE,capacity*PAGE_SIZE);assert(ram);host_page_offset=(uintptr_t)ram;pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0,.end=capacity*PAGE_SIZE,.start_pfn=0,.end_pfn=capacity};pfa.total_frames=capacity;pfa.meta=calloc(capacity,sizeof(*pfa.meta));assert(pfa.meta);spin_init(&pfa.lock);
 device_t dev={.bus=&pci_bus,.plat_data=&hw,.references=1},other={.bus=&pci_bus,.plat_data=&peer,.references=1};virtio_gpu_inst_t *inst=NULL;virtio_transport_t vt={.priv=&hw,.read32=transport_read,.write32=transport_write};
 if(!strncmp(name,"reset-",6)){
  if(!strcmp(name,"reset-parameters")){assert(virtio_transport_reset(NULL,1)==-EINVAL);assert(virtio_transport_reset(&vt,0)==-EINVAL);assert(virtio_transport_reset(&vt,1001)==-EINVAL);vt.read32=NULL;assert(virtio_transport_reset(&vt,1)==-EINVAL);vt.read32=transport_read;vt.write32=NULL;assert(virtio_transport_reset(&vt,1)==-EINVAL);assert(!hw.resets&&!hw.reads);}
  else if(!strcmp(name,"reset-delayed")){hw.delay=7;assert(!virtio_transport_reset(&vt,100)&&hw.resets==1&&hw.reads==8);}
  else if(!strcmp(name,"reset-deadline")){hw.never=1;step=50;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==2);hw.never=0;assert(!virtio_transport_reset(&vt,100)&&hw.resets==2);}
  else if(!strcmp(name,"reset-stalled")){hw.never=1;task_present=1;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==1000000&&yields);hz=0;task_present=0;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==1000000);}
  else {assert(!strcmp(name,"reset-wrap"));now=((uint64_t)-1)-50;step=50;hw.never=1;assert(virtio_transport_reset(&vt,100)==-ETIMEDOUT&&hw.reads==2);hz=((uint64_t)-1);step=0;hw.never=0;hw.delay=3;assert(!virtio_transport_reset(&vt,1000));}
 }else{
  strict_probe=1;
  if(!strcmp(name,"instance-oom")){
   fail_at=1;assert(virtio_gpu_probe(&dev)==-ENOMEM&&!dev.drv_priv&&dev.references==1);fail_at=0;pfa_oom=1;assert(virtio_gpu_probe(&dev)==-ENOMEM&&!dev.drv_priv&&dev.references==1);pfa_oom=0;architecture_ready=0;assert(virtio_gpu_probe(&dev)==-EOPNOTSUPP&&!dev.drv_priv&&dev.references==1);architecture_ready=1;assert(!virtio_gpu_remove(&dev)&&!hw.resets&&!hw.irq_get);
  }
  else if(!strncmp(name,"two-",4)){
   peer.width=96;peer.height=48;assert(!virtio_gpu_probe(&dev)&&!virtio_gpu_probe(&other));inst=dev.drv_priv;virtio_gpu_inst_t *second=other.drv_priv;
   assert(inst&&second&&inst!=second&&inst->fb_memory!=second->fb_memory&&inst->fb_phys!=second->fb_phys&&inst->allocation!=second->allocation);
   assert(!((uintptr_t)inst%__alignof__(virtio_gpu_inst_t))&&!((uintptr_t)second%__alignof__(virtio_gpu_inst_t))&&dev.references==2&&other.references==2);
   assert(inst->width==64&&second->width==96&&hw.irq_get==1&&peer.irq_get==1);
   struct vmo *keep=inst->fb_memory;vmo_ref(keep);pfn_t page=vmo_peek_page(keep,0);((unsigned char *)pfn_to_virt(page))[0]=0x5d;
   if(!strcmp(name,"two-command-fail")){inst->virgl=second->virgl=1;hw.no_complete=1;step=1000;assert(issue(inst,0)==-ETIMEDOUT&&inst->command_failed&&!second->command_failed);step=0;}
   virtio_gpu_quiesce(&dev);if(!strcmp(name,"two-remove-fail")){hw.never=1;step=50;assert(virtio_gpu_remove(&dev)==-ETIMEDOUT&&dev.drv_priv==inst&&other.drv_priv==second&&dev.references==2);}
   unsigned resets=peer.resets,notify=peer.notify;uint16_t available=second->avail.idx;assert(!gpu_flush(&other,0,0,1,1)&&peer.notify==notify+3&&second->avail.idx==(uint16_t)(available+3)&&peer.resets==resets&&!second->closing&&!second->command_failed);
   hw.never=0;assert(!virtio_gpu_remove(&dev)&&!dev.drv_priv&&dev.references==1&&!hw.instance&&other.drv_priv==second);
   assert(((unsigned char *)pfn_to_virt(page))[0]==0x5d&&!gpu_flush(&other,0,0,1,1)&&peer.irq_put==0);
   hw.no_complete=0; /* Repair the injected command stall before rebinding. */
   assert(!virtio_gpu_probe(&dev)&&dev.drv_priv&&dev.drv_priv!=second&&((virtio_gpu_inst_t *)dev.drv_priv)->fb_memory!=keep&&other.drv_priv==second);
   assert(!virtio_gpu_remove(&dev)&&!virtio_gpu_remove(&other)&&dev.references==1&&other.references==1&&hw.irq_put==2&&peer.irq_put==1);vmo_release(keep);
  }
  else if(!strcmp(name,"probe-pci-fail")){hw.pci_error=-ENOMEM;assert(virtio_gpu_probe(&dev)==-ENOMEM&&dev.drv_priv&&opted&&!hw.resets);virtio_gpu_quiesce(&dev);assert(!virtio_gpu_remove(&dev)&&!dev.drv_priv);}
  else if(!strncmp(name,"probe-vmo-",10)){fail_at=!strcmp(name,"probe-vmo-slots-oom")?4:3;assert(virtio_gpu_probe(&dev)<0&&dev.drv_priv&&!((virtio_gpu_inst_t *)dev.drv_priv)->fb_memory&&hw.irq_get==1&&!hw.irq_put);virtio_gpu_quiesce(&dev);hw.never=1;step=50;assert(virtio_gpu_remove(&dev)==-ETIMEDOUT&&!hw.irq_put&&dev.drv_priv);hw.never=0;assert(!virtio_gpu_remove(&dev)&&hw.irq_put==1&&!dev.drv_priv);}
  else if(!strcmp(name,"probe-reset-fail")){hw.never=1;step=50;assert(virtio_gpu_probe(&dev)==-ETIMEDOUT&&dev.drv_priv&&!hw.notify);inst=dev.drv_priv;assert(!virtio_gpu_probe(&other)&&other.drv_priv!=inst&&dev.drv_priv==inst);virtio_gpu_quiesce(&dev);assert(virtio_gpu_remove(&dev)==-ETIMEDOUT&&dev.drv_priv==inst&&!hw.irq_put&&!gpu_flush(&other,0,0,1,1));hw.never=0;assert(!virtio_gpu_remove(&dev)&&!dev.drv_priv&&!virtio_gpu_remove(&other));}
  else{
   if(!strcmp(name,"probe-partial"))hw.bad_response_at=3;
   int probed=virtio_gpu_probe(&dev);inst=dev.drv_priv;assert((!strcmp(name,"probe-partial")?probed<0:probed==0)&&dev.drv_priv&&inst->fb_memory&&hw.irq_get==1);
   struct vmo *snapshot=inst->fb_memory;vmo_ref(snapshot);pfn_t page=vmo_peek_page(snapshot,0);((unsigned char *)pfn_to_virt(page))[0]=0x7a;size_t pages=allocated_pages,instance_pages=(size_t)1<<inst->allocation->order;
   assert(virtio_gpu_probe(&dev)==-EBUSY&&dev.drv_priv==inst&&inst->fb_memory==snapshot);
   if(!strcmp(name,"capabilities")){struct gpu_capabilities capabilities={GPU_CAP_KNOWN};assert(gpu_get_capabilities(NULL,&capabilities)==-ENODEV&&!capabilities.flags);assert(gpu_get_capabilities(&dev,NULL)==-EINVAL);assert(!gpu_get_capabilities(&dev,&capabilities)&&capabilities.flags==(GPU_CAP_SCANOUT|GPU_CAP_RAM_SCANOUT));inst->virgl=1;assert(!gpu_get_capabilities(&dev,&capabilities)&&capabilities.flags==(GPU_CAP_SCANOUT|GPU_CAP_RAM_SCANOUT|GPU_CAP_RENDER));inst->valid=0;capabilities.flags=GPU_CAP_KNOWN;assert(gpu_get_capabilities(&dev,&capabilities)==-ENODEV&&!capabilities.flags);inst->valid=1;virtio_gpu_quiesce(&dev);capabilities.flags=GPU_CAP_KNOWN;assert(gpu_get_capabilities(&dev,&capabilities)==-ENODEV&&!capabilities.flags);}
   else if(!strncmp(name,"command-",8)){
    unsigned path=(unsigned)(name[strlen(name)-1]-'0');inst->virgl=1;
    if(strstr(name,"oom")){unsigned notify=hw.notify;uint16_t idx=inst->avail.idx;fail_at=attempts+1;assert(issue(inst,path)==-ENOMEM&&!inst->command_failed&&!inst->command_active&&hw.notify==notify&&inst->avail.idx==idx);fail_at=0;assert(!issue(inst,path));}
    else if(strstr(name,"timeout")){hw.no_complete=1;step=1000;assert(issue(inst,path)==-ETIMEDOUT&&inst->command_failed);}
    else if(strstr(name,"wrong-id")){hw.bad_id=7;assert(issue(inst,path)==-EIO&&inst->command_failed);}
    else if(strstr(name,"wrong-index")){hw.bad_advance=2;assert(issue(inst,path)==-EIO&&inst->command_failed);}
    else{assert(strstr(name,"success"));assert(!issue(inst,path)&&!inst->command_failed);}
    if(inst->command_failed){unsigned notify=hw.notify;size_t allocs=attempts;uint16_t idx=inst->avail.idx;for(unsigned j=0;j<4;j++)assert(issue(inst,j)==-EIO);assert(hw.notify==notify&&attempts==allocs&&inst->avail.idx==idx&&!inst->command_active);}
   }else if(!strcmp(name,"remove-active")){assert(!virtio_gpu_command_begin(inst));virtio_gpu_quiesce(&dev);assert(virtio_gpu_remove(&dev)==-EBUSY&&hw.resets==1&&!hw.irq_put&&inst->fb_memory==snapshot);virtio_gpu_command_end(inst);}
   else assert(!strcmp(name,"remove-retained")||!strcmp(name,"probe-partial"));
   virtio_gpu_quiesce(&dev);unsigned notify=hw.notify;inst->virgl=1;for(unsigned j=0;j<4;j++)assert(issue(inst,j)==-ENODEV);assert(hw.notify==notify);
   hw.never=1;step=50;assert(virtio_gpu_remove(&dev)==-ETIMEDOUT&&dev.drv_priv&&inst->fb_memory==snapshot&&!hw.irq_put&&allocated_pages==pages);
   hw.never=0;assert(!virtio_gpu_remove(&dev)&&!dev.drv_priv&&!selected&&hw.irq_put==1&&allocated_pages==pages-instance_pages);
   assert(((unsigned char *)pfn_to_virt(page))[0]==0x7a);vmo_release(snapshot);assert(!allocated_pages);assert(!virtio_gpu_remove(&dev)&&hw.irq_put==1);
  }
 }
 assert(dev.references==1&&other.references==1);assert(!heap_live&&!allocated_pages&&!g_a20_objstats.vmos&&!g_a20_objstats.vmo_pages);free(pfa.meta);free(ram);return 0;
}

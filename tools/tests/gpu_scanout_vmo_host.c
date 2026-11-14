/* Appended to the contiguous VMO/PFA fixture. Real owning-snapshot and fbdev
 * adapter bodies; the locked VMO map is modeled, class admission is separate. */
#include "drivers/gpu/gpu_core.h"
#include "drivers/core/driver_class.h"
#include "core/mman.h"
#ifndef USER_VA_LIMIT
#define USER_VA_LIMIT 0x10000000UL
#endif
#define ROUND_UP(v,a) (((v)+(a)-1)&~((a)-1))
typedef struct {spinlock_t lock;int active,fail;struct vmo *memory;uint64_t offset;size_t size;int prot,flags;uint32_t cap;} mm_struct_t;
static int mm_addr_is_error(vaddr_t a){return (intptr_t)a<0;}
static int mm_addr_error(vaddr_t a){return (int)(intptr_t)a;}
static void mm_tlb_invalidate_begin(mm_struct_t *m){assert(!m->active);assert(!pthread_mutex_trylock(&m->lock.native));assert(!pthread_mutex_unlock(&m->lock.native));m->active=1;}
static void mm_tlb_invalidate_finish(mm_struct_t *m){assert(m->active);assert(!pthread_mutex_trylock(&m->lock.native));assert(!pthread_mutex_unlock(&m->lock.native));m->active=0;}
static void mm_vma_flush_deferred(mm_struct_t *m){assert(!m->active);}
static pte_t mm_prot_to_pte_flags(int prot){return prot|((prot&PROT_WRITE)?PROT_READ:0);}
static int mm_pte_flags_to_prot(pte_t flags){return flags;}
static vaddr_t mm_mmap_vmo_cap_locked(mm_struct_t *mm,vaddr_t addr,size_t len,int prot,int flags,struct vmo *v,uint64_t off,uint32_t cap,void *owner){
 assert(mm->active&&pthread_mutex_trylock(&mm->lock.native)==EBUSY&&!owner);
 if(mm->fail)return (vaddr_t)mm->fail;
 assert(!mm->memory);vmo_ref(v);mm->memory=v;mm->offset=off;mm->size=len;mm->prot=prot;mm->flags=flags;mm->cap=cap;return addr?addr:0x20000;
}
/* PRODUCTION_MAP_CAP */
typedef struct {mm_struct_t *mm;} task_t;
static task_t task;
static task_t *proc_current(void){return &task;}
static unsigned kicks;
static void fb_autoflush_kick(void){kicks++;}
/* PRODUCTION_SNAPSHOT */
/* PRODUCTION_FB_SCANOUT */
struct provider {struct vmo *memory;unsigned mode,calls;};
static int snapshot(struct device *dev,struct gpu_scanout *out){
 struct provider *p=dev->drv_priv;p->calls++;*out=(struct gpu_scanout){0};if(p->mode==1)return 0;
 vmo_ref(p->memory);*out=(struct gpu_scanout){p->memory,PAGE_SIZE,PAGE_SIZE+33};
 switch(p->mode){case 2:out->offset++;out->size=33;break;case 3:out->size=0;break;case 4:out->offset=p->memory->size;break;case 5:out->size=p->memory->size;break;case 6:out->size=SIZE_MAX;break;case 7:return -ETIMEDOUT;case 8:return 1;}
 return 0;
}
/* Only the producer fields consumed by the real get/release/remove bodies.
 * Transport/IRQ operations are stubs; this does not prove hardware stop. */
typedef struct fake_transport {void (*write32)(struct fake_transport *,uint32_t,uint32_t);int irq;} fake_transport_t;
typedef pthread_mutex_t mutex_t;
static int mutex_trylock(mutex_t *m){return !pthread_mutex_trylock(m);}
static void mutex_unlock(mutex_t *m){assert(!pthread_mutex_unlock(m));}
struct driver_dma_buffer { void *data; device_t *device; };
static int virtio_transport_reset(fake_transport_t *t,uint32_t ms){assert(ms==100);t->write32(t,0x70,0);return 0;}
typedef struct {struct driver_dma_buffer *allocation;struct driver_queue *commands;mutex_t command_lock;int closing,command_active,transport_live,reset_confirmed;fake_transport_t vt;int valid,virgl,context_init,irq_registered;uint32_t width,height,bpp;uintptr_t fb_phys;size_t fb_size;struct vmo *fb_memory;void *big_req,*big_resp;size_t big_req_cap,big_resp_cap;uint16_t last_used;} virtio_gpu_inst_t;
#define VIRTIO_MMIO_STATUS 0x70
#define mb() ((void)0)
static struct gpu_scanout *observed_snapshot;
static void observe_release(void *obj){(void)obj;assert(observed_snapshot&&!observed_snapshot->memory&&!observed_snapshot->size&&!observed_snapshot->offset);}
static unsigned resets,irq_frees,unregisters;
static void reset_write(fake_transport_t *t,uint32_t reg,uint32_t value){assert(t->irq==5&&reg==0x70&&!value);resets++;}
static void free_irq(uint32_t irq,void *arg){assert(irq==5&&((virtio_gpu_inst_t *)arg)->fb_memory);irq_frees++;}
void gpu_device_unregister(device_t *d){assert(d&&d->drv_priv&&((virtio_gpu_inst_t *)d->drv_priv)->fb_memory);unregisters++;}
/* Native allocation final put is a boundary model here. The complete native
 * buffer allocator and two-device owners run in test_virtio_gpu_stop. */
static unsigned instance_puts;
static void driver_dma_buffer_put(struct driver_dma_buffer *allocation){
 assert(allocation&&allocation->data&&!allocation->device->drv_priv);
 virtio_gpu_inst_t *inst=allocation->data;
 assert(!inst->fb_memory&&!inst->valid&&inst->reset_confirmed&&resets==1&&irq_frees==1&&unregisters==1);
 assert(!pthread_mutex_destroy(&inst->command_lock));free(inst);allocation->data=NULL;instance_puts++;
}
/* This fixture isolates RAM scanout ownership; queue execution has its own
 * production-core and full GPU transport fixtures. No command queue here. */
static int driver_queue_stop(struct driver_queue *q){(void)q;abort();}
static void driver_queue_put(struct driver_queue *q){assert(!q);}
/* PRODUCTION_VIRTIO */
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];size_t capacity=(size_t)1<<MAX_ORDER;
 unsigned char *ram=aligned_alloc(PAGE_SIZE,capacity*PAGE_SIZE);assert(ram);host_page_offset=(uintptr_t)ram;
 pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0,.end=capacity*PAGE_SIZE,.start_pfn=0,.end_pfn=capacity};pfa.total_frames=capacity;pfa.meta=calloc(capacity,sizeof(*pfa.meta));assert(pfa.meta);spin_init(&pfa.lock);
 struct provider provider={0};gpu_dev_ops_t *ops=calloc(1,sizeof(*ops));assert(ops);ops->get_scanout=snapshot;
 struct device device={.drv_priv=&provider};struct gpu_device_access access={(void *)1,&device,ops};
 struct gpu_scanout view;
 if(!strcmp(name,"arguments")){
  view=(struct gpu_scanout){(void *)1,9,9};assert(gpu_device_scanout_get(NULL,&view)==-ENODEV&&!view.memory&&!view.size);
  assert(gpu_device_scanout_get(&access,NULL)==-EINVAL);access.publication=NULL;assert(gpu_device_scanout_get(&access,&view)==-ENODEV);access.publication=(void *)1;
  ops->get_scanout=NULL;assert(gpu_device_scanout_get(&access,&view)==-EOPNOTSUPP&&!view.memory);gpu_scanout_put(NULL);gpu_scanout_put(&view);gpu_scanout_put(&view);
 }else if(!strcmp(name,"virtio-provider")){
  virtio_gpu_inst_t *inst=calloc(1,sizeof(*inst));assert(inst);assert(!pthread_mutex_init(&inst->command_lock,NULL));inst->transport_live=1;
  assert(!vmo_create_contiguous(3*PAGE_SIZE,&inst->fb_memory));inst->valid=1;inst->fb_size=3*PAGE_SIZE;
  inst->big_req=kmalloc(16);inst->big_resp=kmalloc(16);inst->vt=(fake_transport_t){reset_write,5};inst->irq_registered=1;
  struct driver_dma_buffer allocation={inst,&device};inst->allocation=&allocation;
  device.drv_priv=inst;ops->get_scanout=gpu_get_scanout;
  assert(!gpu_device_scanout_get(&access,&view)&&view.memory==inst->fb_memory);
  assert(!virtio_gpu_remove(&device)&&!device.drv_priv&&!allocation.data&&instance_puts==1&&resets==1&&irq_frees==1&&unregisters==1);
  free(ops);ops=NULL;uint64_t pa;size_t bytes;
  assert(!vmo_contiguous_info(view.memory,&pa,&bytes)&&bytes==3*PAGE_SIZE&&allocated_pages==3);
  gpu_scanout_put(&view);assert(!allocated_pages);
 }else if(!strcmp(name,"invalid")){
  for(unsigned mode=1;mode<=9;mode++){
   provider.mode=mode;assert(!vmo_create_contiguous(3*PAGE_SIZE,&provider.memory));if(mode==9)provider.memory->type=VMO_ANONYMOUS;
   int expected=mode==7?-ETIMEDOUT:mode==8?-EIO:-EINVAL;
   assert(gpu_device_scanout_get(&access,&view)==expected&&!view.memory&&!view.size&&!view.offset);
   vmo_release(provider.memory);provider.memory=NULL;assert(!allocated_pages&&!g_a20_objstats.vmos);
  }
 }else{
  assert(!vmo_create_contiguous(3*PAGE_SIZE,&provider.memory));ram[2*PAGE_SIZE]=0x6a;
  if(!strcmp(name,"detached")){
   assert(!gpu_device_scanout_get(&access,&view)&&view.memory==provider.memory&&view.offset==PAGE_SIZE);
   vmo_release(provider.memory);provider.memory=NULL;free(ops);ops=NULL;device.drv_priv=NULL;access=(struct gpu_device_access){0};
   uint64_t physical;size_t bytes;assert(!vmo_contiguous_info(view.memory,&physical,&bytes)&&!physical&&bytes==3*PAGE_SIZE&&ram[2*PAGE_SIZE]==0x6a);
   observed_snapshot=&view;host_destroy_hook=observe_release;
   gpu_scanout_put(&view);host_destroy_hook=NULL;assert(!view.memory&&!view.size&&!allocated_pages);gpu_scanout_put(&view);
  }else{
   mm_struct_t mm={0};spin_init(&mm.lock);task.mm=&mm;
   if(!strcmp(name,"cap-validation")){
    assert((intptr_t)mm_mmap_vmo_cap(NULL,0,PAGE_SIZE,1,MAP_SHARED,provider.memory,0,1)==-EINVAL);
    assert((intptr_t)mm_mmap_vmo_cap(&mm,0,PAGE_SIZE,1,MAP_SHARED,NULL,0,1)==-EINVAL);
    assert((intptr_t)mm_mmap_vmo_cap(&mm,0,PAGE_SIZE,3,MAP_SHARED,provider.memory,0,1)==-EACCES);
    assert((intptr_t)mm_mmap_vmo_cap(&mm,0,PAGE_SIZE,8,MAP_SHARED,provider.memory,0,7)==-EINVAL);
    assert((intptr_t)mm_mmap_vmo_cap(&mm,0,PAGE_SIZE,1,MAP_SHARED,provider.memory,0,8)==-EINVAL);
    assert(!mm.active&&!mm.memory);
    assert(mm_mmap_vmo_cap(&mm,0,PAGE_SIZE,1,MAP_SHARED,provider.memory,0,1)==0x20000);
    assert(mm.cap==1&&mm.prot==1&&!mm.active);vmo_release(provider.memory);provider.memory=NULL;vmo_release(mm.memory);mm.memory=NULL;
   }else if(!strcmp(name,"map")){
    assert(fb_map_scanout(&access,0,PAGE_SIZE,PROT_WRITE,MAP_SHARED,PAGE_SIZE,false)==0x20000);
    assert(mm.memory==provider.memory&&mm.offset==2*PAGE_SIZE&&mm.size==PAGE_SIZE&&mm.prot==3&&mm.cap==3&&kicks==1);
    vmo_release(provider.memory);provider.memory=NULL;free(ops);ops=NULL;device.drv_priv=NULL;
    assert(allocated_pages==3&&ram[2*PAGE_SIZE]==0x6a);vmo_release(mm.memory);mm.memory=NULL;
   }else if(!strcmp(name,"map-failures")){
    assert(fb_map_scanout(&access,0,0,3,MAP_SHARED,0,false)==-EINVAL);
    assert(fb_map_scanout(&access,0,3*PAGE_SIZE,3,MAP_SHARED,0,false)==-ENXIO);
    assert(fb_map_scanout(&access,0,PAGE_SIZE,7,MAP_SHARED,0,false)==-EACCES);
    assert(fb_map_scanout(&access,0,PAGE_SIZE,3,MAP_PRIVATE,0,false)==-EINVAL);
    assert(fb_map_scanout(&access,1,PAGE_SIZE,3,MAP_SHARED|MAP_FIXED,0,false)==-EINVAL);
    assert(fb_map_scanout(&access,USER_VA_LIMIT-PAGE_SIZE,2*PAGE_SIZE,3,MAP_SHARED|MAP_FIXED,0,false)==-EINVAL);
    task.mm=NULL;assert(fb_map_scanout(&access,0,PAGE_SIZE,3,MAP_SHARED,0,false)==-EFAULT);task.mm=&mm;
    mm.fail=-ENOMEM;assert(fb_map_scanout(&access,0,PAGE_SIZE,3,MAP_SHARED,0,false)==-ENOMEM&&!mm.memory&&!mm.active);
    mm.fail=0;assert(!provider.mode);assert(fb_map_scanout(&access,0x30000,0,3,MAP_SHARED|MAP_FIXED_NOREPLACE,0,true)==0x30000&&mm.size==2*PAGE_SIZE&&mm.offset==PAGE_SIZE);
    vmo_release(mm.memory);mm.memory=NULL;vmo_release(provider.memory);provider.memory=NULL;
   }else assert(0);
  }
 }
 free(ops);assert(!heap_live&&!allocated_pages&&!g_a20_objstats.vmos&&!g_a20_objstats.vmo_pages);free(pfa.meta);free(ram);return 0;
}

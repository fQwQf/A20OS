/* Production Rust VMO snapshot + C primitives, mapper and VMO DMA adapter. */
#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "mm/vmo.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/dma_mapping.h"
#include "core/errno.h"

pfa_t pfa;
_Thread_local spinlock_t *host_owned_spinlock;
static _Atomic unsigned refs[256], frees[256], gets, put_count, cpu_syncs, device_syncs;
static unsigned allocations, attempts, fail_at, destroys;
static unsigned char contents[256];
static struct vmo *source;
static int ready=1, resize_on_map, wait_resize;
static pthread_mutex_t rendezvous=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static int pin_pause,pin_entered,pin_proceed,resize_started,resize_done;
void *kmalloc(size_t n) {
 if(++attempts==fail_at)return NULL;
 void *p=malloc(n);assert(p);allocations++;return p;
}
void kfree(void *p) {if(p){assert(allocations);allocations--;free(p);}}
device_t *device_get(device_t *d) {assert(d->references);d->references++;return d;}
void device_put(device_t *d) {assert(d->references);d->references--;}
uint64_t dma_get_mask(const device_t *d) {(void)d;return ~(uint64_t)0;}
int arch_device_dma_ready(void) {return ready;}
void frame_get(pfn_t p) {assert(pfn_valid(p)&&refs[p]);refs[p]++;gets++;}
void frame_put(pfn_t p) {assert(pfn_valid(p)&&refs[p]);put_count++;if(!--refs[p]){frees[p]++;contents[p]=0xde;}}
void frame_get_many(const pfn_t *pages,size_t count) {
 if(pin_pause) {
  assert(!pthread_mutex_lock(&rendezvous));pin_entered=1;assert(!pthread_cond_broadcast(&changed));
  while(!pin_proceed)assert(!pthread_cond_wait(&changed,&rendezvous));
  assert(!pthread_mutex_unlock(&rendezvous));
 }
 for(size_t i=0;i<count;i++)frame_get(pages[i]);
}
void frame_put_many(const pfn_t *pages,size_t count) {for(size_t i=0;i<count;i++)frame_put(pages[i]);}
static void unlocked(void) {
 /* Another thread may resize concurrently. Only this callback's lock
  * ownership would violate the provider/cache contract. */
 assert(host_owned_spinlock != &source->lock);
}
void dma_sync_for_cpu(void *p,size_t n) {assert(p&&n==PAGE_SIZE);unlocked();cpu_syncs++;}
void dma_sync_for_device(void *p,size_t n) {assert(p&&n==PAGE_SIZE);unlocked();device_syncs++;}
/* The sparse index is modeled; type/locking/slot dispatch is production C. */
pfn_t a20_vmo_sparse_peek(void *root,uint32_t index) {return ((pfn_t *)root)[index];}
/* PRODUCTION_VMO_PRIMITIVES */
static void shrink_source(void) {
 uint64_t flags=spin_lock_irqsave(&source->lock);
 for(unsigned i=0;i<source->page_count;i++)if(source->pages[i]!=PFN_NONE){frame_put(source->pages[i]);source->pages[i]=PFN_NONE;}
 source->page_count=0;source->size=0;spin_unlock_irqrestore(&source->lock,flags);
}
/* Destruction/resizing side effects are modeled; public refs and snapshot
 * decisions run the production Rust lifecycle object. */
void a20_vmo_destroy_rust(struct vmo *vmo) {
 assert(vmo==source);shrink_source();destroys++;kfree(vmo->pages);
 /* Leave metadata for cache-hook lock assertions until the final mapping put. */
}
extern void a20_vmo_ref_rust(struct vmo *);
extern void a20_vmo_release_rust(struct vmo *);

struct provider {
 uint64_t address[8];unsigned calls,live,unmaps,releases,detaches,fail_map,fail_unmap;
 int pending;
};
static int map(void *opaque,uint64_t physical,enum driver_dma_direction direction,
                uint64_t *address,bool *needs_unmap) {
 struct provider *p=opaque;assert(direction==DRIVER_DMA_BIDIRECTIONAL);unlocked();
 if(resize_on_map&&!p->calls)shrink_source();
 if(wait_resize) {
  assert(!pthread_mutex_lock(&rendezvous));while(!resize_done)assert(!pthread_cond_wait(&changed,&rendezvous));
  assert(!pthread_mutex_unlock(&rendezvous));
 }
 pfn_t frame=phys_to_pfn(physical);assert(frame==11+p->calls&&refs[frame]);
 p->calls++;*needs_unmap=false;
 if(p->calls==p->fail_map&&!p->pending)return -EIO;
 *address=0x10000+p->calls*2*PAGE_SIZE;*needs_unmap=true;p->address[p->live++]=*address;
 return p->calls==p->fail_map?-EIO:0;
}
static int unmap(void *opaque,uint64_t address,enum driver_dma_direction direction) {
 struct provider *p=opaque;assert(direction==DRIVER_DMA_BIDIRECTIONAL);unlocked();
 assert(p->live&&address==p->address[p->live-1]);
 if(++p->unmaps==p->fail_unmap)return -ETIMEDOUT;
 p->live--;return 0;
}
static int deactivate(void *opaque) {struct provider *p=opaque;assert(!p->live);p->detaches++;return 0;}
static void release(void *opaque) {struct provider *p=opaque;assert(!p->live);p->releases++;}
static const struct driver_dma_managed_ops ops={map,unmap,deactivate,release};
static void make_source(void) {
 source=kmalloc(sizeof(*source));memset(source,0,sizeof(*source));refcount_set(&source->refcount,1);spin_init(&source->lock);
 source->pages=kmalloc(3*sizeof(pfn_t));source->page_count=3;source->size=2*PAGE_SIZE+33;
 for(unsigned i=0;i<3;i++){source->pages[i]=10+i;refs[10+i]=1;contents[10+i]=0x55;}
 source->sparse_root=source->pages;
}
static struct driver_dma_domain *thread_domain;
static struct driver_dma_mapping *thread_mapping;
static int thread_result;
static void *mapper(void *arg) {
 (void)arg;thread_result=driver_dma_map_vmo(thread_domain,source,1,2,DRIVER_DMA_BIDIRECTIONAL,&thread_mapping);return NULL;
}
static void *resizer(void *arg) {
 (void)arg;__atomic_store_n(&resize_started,1,__ATOMIC_RELEASE);shrink_source();
 assert(!pthread_mutex_lock(&rendezvous));resize_done=1;assert(!pthread_cond_broadcast(&changed));
 assert(!pthread_mutex_unlock(&rendezvous));return NULL;
}

static void *hold_source_lock(void *arg) {
 (void)arg;uint64_t flags=spin_lock_irqsave(&source->lock);
 assert(!pthread_mutex_lock(&rendezvous));pin_entered=1;assert(!pthread_cond_broadcast(&changed));
 while(!pin_proceed)assert(!pthread_cond_wait(&changed,&rendezvous));
 assert(!pthread_mutex_unlock(&rendezvous));spin_unlock_irqrestore(&source->lock,flags);return NULL;
}

int main(int argc,char **argv) {
 assert(argc==2);const char *name=argv[1];
 pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0x400000,.end=0x500000,.start_pfn=0,.end_pfn=256};
 make_source();device_t device={.references=1};struct provider provider={0};struct driver_dma_domain *domain;
 struct driver_dma_mapping *mapping=(void *)1;struct driver_dma_policy *policy;
 if(!strcmp(name,"policy")) {
  struct driver_dma_rule rule={pfn_to_phys(12)+32,pfn_to_phys(12)+PAGE_SIZE-1,0,0};
  assert(!driver_dma_policy_create(&rule,1,&policy));
  assert(!driver_dma_domain_create_managed_policy(&device,&ops,&provider,policy,&domain));driver_dma_policy_put(policy);
 } else assert(!driver_dma_domain_create_managed(&device,&ops,&provider,&domain));
 if(!strcmp(name,"observer")) {
  pthread_t holder;assert(!pthread_create(&holder,NULL,hold_source_lock,NULL));
  assert(!pthread_mutex_lock(&rendezvous));while(!pin_entered)assert(!pthread_cond_wait(&changed,&rendezvous));
  assert(!pthread_mutex_unlock(&rendezvous));
  dma_sync_for_device((void *)1,PAGE_SIZE);dma_sync_for_cpu((void *)1,PAGE_SIZE);
  assert(!pthread_mutex_lock(&rendezvous));pin_proceed=1;assert(!pthread_cond_broadcast(&changed));
  assert(!pthread_mutex_unlock(&rendezvous));assert(!pthread_join(holder,NULL));
 } else if(!strcmp(name,"owned-lock")) {
  uint64_t flags=spin_lock_irqsave(&source->lock);unlocked();spin_unlock_irqrestore(&source->lock,flags);
 } else if(!strcmp(name,"parameters")) {
  unsigned before=attempts;
  assert(driver_dma_map_vmo(NULL,source,1,2,2,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_vmo(domain,NULL,1,2,2,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_vmo(domain,source,1,0,2,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_vmo(domain,source,1,2,99,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_vmo(domain,source,1,2,2,NULL)==-EINVAL);
  assert(driver_dma_map_vmo(domain,source,0,VMO_PIN_MAX_PAGES+1,2,&mapping)==-E2BIG&&!mapping);
  assert(attempts==before&&!gets&&!provider.calls);
  assert(driver_dma_map_vmo(domain,source,2,2,2,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_vmo(domain,source,(uint32_t)-1,2,2,&mapping)==-EINVAL&&!mapping);
 } else if(!strcmp(name,"physical")) {
  for(unsigned kind=1;kind<100;kind+=98) {
   source->type=kind;assert(driver_dma_map_vmo(domain,source,1,2,2,&mapping)==-EOPNOTSUPP&&!mapping&&!gets);
  }
 } else if(!strcmp(name,"hole")) {
  for(unsigned kind=0;kind<4;kind++)if(kind!=1) {
   source->type=kind;source->pages[2]=PFN_NONE;pfn_t pages[2]={77,88};
   assert(vmo_pin_pages(source,1,2,pages)==-EAGAIN&&pages[0]==77&&pages[1]==88&&!gets);
   assert(driver_dma_map_vmo(domain,source,1,2,2,&mapping)==-EAGAIN&&!mapping&&!gets&&!provider.calls);
  }
  source->pages[2]=12;
 } else if(!strcmp(name,"oom")) {
  for(unsigned fail=1;fail<=2;fail++) {
   fail_at=attempts+fail;assert(driver_dma_map_vmo(domain,source,1,2,2,&mapping)==-ENOMEM&&!mapping);
   assert(refs[11]==1&&refs[12]==1&&!provider.calls);fail_at=0;
  }
 } else if(!strcmp(name,"policy")||!strcmp(name,"closed")||!strcmp(name,"architecture")) {
  int error=-EACCES;
  if(!strcmp(name,"closed")){assert(!driver_dma_domain_shutdown(domain));error=-ESHUTDOWN;}
  if(!strcmp(name,"architecture")){ready=0;error=-EOPNOTSUPP;}
  assert(driver_dma_map_vmo(domain,source,1,2,2,&mapping)==error&&!mapping&&!provider.calls);
  assert(refs[11]==1&&refs[12]==1&&gets==put_count);
 } else {
  if(!strcmp(name,"paged"))source->type=VMO_PAGED;
  if(!strcmp(name,"sparse"))source->type=VMO_SHARED_ANON;
  if(!strcmp(name,"resize"))resize_on_map=1;
  if(!strcmp(name,"rollback")){provider.fail_map=2;provider.pending=1;provider.fail_unmap=1;}
  if(!strcmp(name,"race")) {
   pin_pause=wait_resize=1;thread_domain=domain;pthread_t a,b;
   assert(!pthread_create(&a,NULL,mapper,NULL));assert(!pthread_mutex_lock(&rendezvous));
   while(!pin_entered)assert(!pthread_cond_wait(&changed,&rendezvous));
   assert(pthread_mutex_trylock(&source->lock.native)==EBUSY);
   assert(!pthread_create(&b,NULL,resizer,NULL));
   while(!__atomic_load_n(&resize_started,__ATOMIC_ACQUIRE))sched_yield();
   assert(!resize_done);pin_proceed=1;assert(!pthread_cond_broadcast(&changed));assert(!pthread_mutex_unlock(&rendezvous));
   assert(!pthread_join(a,NULL)&&!pthread_join(b,NULL));assert(!thread_result);mapping=thread_mapping;
  } else {
   int ret=driver_dma_map_vmo(domain,source,1,2,2,&mapping);
   assert(!strcmp(name,"rollback")?ret==-EIO:!ret);
  }
  assert(mapping&&driver_dma_mapping_count(mapping)==2);
  a20_vmo_release_rust(source);assert(destroys==1&&refs[11]==1&&refs[12]==1&&!frees[11]&&!frees[12]);
  assert(!refs[10]);contents[11]=0xa1;contents[12]=0xb2;
  if(!strcmp(name,"rollback")) {
   uint64_t address;assert(driver_dma_page_address(mapping,0,&address)==-ESHUTDOWN&&!cpu_syncs&&!device_syncs);
   assert(driver_dma_domain_shutdown(domain)==-EBUSY);
  }
  if(!strcmp(name,"unmap")) {
   provider.fail_unmap=2;assert(driver_dma_try_unmap_pages(&mapping)==-ETIMEDOUT&&mapping&&provider.live==1);
   assert(refs[11]==1&&refs[12]==1&&!cpu_syncs&&contents[11]==0xa1);
  }
  assert(!driver_dma_try_unmap_pages(&mapping)&&!mapping&&!refs[11]&&!refs[12]);
  assert(cpu_syncs==(!strcmp(name,"rollback")?0U:2U));
 }
 if(!destroys)a20_vmo_release_rust(source);
 assert(!driver_dma_domain_shutdown(domain));driver_dma_domain_put(domain);
 assert(provider.releases==1&&provider.detaches==1&&device.references==1);
 assert(!refs[10]&&!refs[11]&&!refs[12]&&gets+3==put_count);
 kfree(source);assert(!allocations);puts("native VMO DMA ownership PASS");return 0;
}

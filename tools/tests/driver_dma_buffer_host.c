/* Production native allocation owner + real device references/registry. */
#include "core/refcount.h"
#include "mm/frame.h"
#include <time.h>
pfa_t pfa;
static unsigned page_calls,limited_calls,page_frees;
static int selected_order,freed_order,page_fail,bad_alias,race;
static pfn_t chosen=5,freed_pfn;
static device_t *allocation_owner;
static unsigned char small_memory[65536],large_memory[PAGE_SIZE<<MAX_ORDER];
static int dma_ready=1;
extern int arch_device_dma_ready(void) __attribute__((weak));
static void *buffer_alias(pfn_t pfn) {assert(pfn==chosen);return bad_alias?NULL:selected_order<=4?small_memory:large_memory;}
#define pfn_to_virt buffer_alias
pfn_t pfa_alloc(int order) {
 page_calls++;selected_order=order;
 if(race==1)allocation_owner->state=DEV_STATE_REMOVING;
 if(race==2)allocation_owner->dma_mask=0xffffffULL;
 if(race==3)assert(!device_unregister(allocation_owner));
 return page_fail?PFN_NONE:chosen;
}
pfn_t pfa_alloc_max(int order,uint64_t max) {assert(max==allocation_owner->dma_mask);limited_calls++;return pfa_alloc(order);}
void pfa_free(pfn_t pfn,int order) {assert(pfn==chosen&&order==selected_order);page_frees++;freed_order=order;freed_pfn=pfn;}
/* PRODUCTION_BUFFER */
int arch_device_dma_ready(void) {return dma_ready;}
static unsigned owner_releases;
static void buffer_owner_release(device_t *d) {owner_releases++;kfree(d);}
static device_t *buffer_owner(void) {
 device_t *d=kcalloc(1,sizeof(*d));assert(d);d->references=1;d->name="dma-buffer";d->release=buffer_owner_release;
 assert(!device_register(d));allocation_owner=d;return d;
}
static void buffer_destroy_owner(device_t *d) {if(device_is_registered(d))assert(!device_unregister(d));device_put(d);}
static struct driver_dma_buffer *thread_buffer;
static void *buffer_refs(void *arg) {
 (void)arg;for(unsigned i=0;i<10000;i++){driver_dma_buffer_get(thread_buffer);assert(driver_dma_buffer_size(thread_buffer)==33);driver_dma_buffer_put(thread_buffer);}return NULL;
}
static int create_result;
static void *buffer_create_worker(void *arg) {(void)arg;create_result=driver_dma_buffer_create(allocation_owner,33,65536,&thread_buffer);return NULL;}
static void dma_buffer_case(const char *name) {
 pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0x8800b000,.end=0x8900b000,.start_pfn=0,.end_pfn=4096};
 device_t *d=buffer_owner();unsigned refs=d->references;struct driver_dma_buffer *b=(void *)1;
 if(!strcmp(name,"parameters")) {
  assert(driver_dma_buffer_create(d,0,64,&b)==-EINVAL&&!b);
  assert(driver_dma_buffer_create(d,33,0,&b)==-EINVAL&&!b);
  assert(driver_dma_buffer_create(d,33,3,&b)==-EINVAL&&!b);
  assert(driver_dma_buffer_create(NULL,33,64,&b)==-EINVAL&&!b);
  assert(driver_dma_buffer_create(d,33,64,NULL)==-EINVAL);
  assert(driver_dma_buffer_create(d,(size_t)-1,64,&b)==-E2BIG&&!b);
  assert(driver_dma_buffer_create(d,1,((size_t)PAGE_SIZE<<MAX_ORDER)*2,&b)==-E2BIG&&!b);
  d->dma_mask=0x1234;assert(driver_dma_buffer_create(d,33,64,&b)==-EINVAL&&!b);d->dma_mask=0;
  dma_ready=0;assert(driver_dma_buffer_create(d,33,64,&b)==-EOPNOTSUPP&&!b);dma_ready=1;
  d->state=DEV_STATE_PREPARING;assert(driver_dma_buffer_create(d,33,64,&b)==-ENODEV&&!b);d->state=DEV_STATE_UNINIT;
  assert(!device_unregister(d));assert(driver_dma_buffer_create(d,33,64,&b)==-ENODEV&&!b);
  assert(!page_calls&&d->references==1);device_put(d);return;
 }
 if(!strcmp(name,"allocation")) {
  fail_at=allocations+1;assert(driver_dma_buffer_create(d,33,65536,&b)==-ENOMEM&&!b&&!page_calls&&d->references==refs);fail_at=0;
  page_fail=1;assert(driver_dma_buffer_create(d,33,65536,&b)==-ENOMEM&&!b&&!page_frees&&d->references==refs);page_fail=0;
  chosen=6;assert(driver_dma_buffer_create(d,33,65536,&b)==-ERANGE&&!b&&page_frees==1);chosen=5;
  d->dma_mask=0xffff;assert(driver_dma_buffer_create(d,33,64,&b)==-ERANGE&&!b&&limited_calls==1&&page_frees==2);
  d->dma_mask=0;bad_alias=1;assert(driver_dma_buffer_create(d,33,64,&b)==-ERANGE&&!b&&page_frees==3);bad_alias=0;
  assert(d->references==refs);buffer_destroy_owner(d);return;
 }
 if(!strcmp(name,"races")) {
  race=1;assert(driver_dma_buffer_create(d,33,65536,&b)==-ENODEV&&!b&&page_frees==1&&d->references==refs);
  d->state=DEV_STATE_UNINIT;race=2;assert(driver_dma_buffer_create(d,33,65536,&b)==-EAGAIN&&!b&&page_frees==2&&d->references==refs);
  d->dma_mask=0;race=3;assert(driver_dma_buffer_create(d,33,65536,&b)==-ENODEV&&!b&&page_frees==3&&d->references==refs-1);
  buffer_destroy_owner(d);return;
 }
 if(!strcmp(name,"zero")) {
  chosen=0;pfa.ranges[0].base=0;pfa.ranges[0].end=0x1000000;
  assert(!driver_dma_buffer_create(d,33,1,&b)&&driver_dma_buffer_address(b)==0&&driver_dma_buffer_data(b));
  driver_dma_buffer_put(b);assert(page_frees==1&&freed_order==0);
  d->dma_mask=31;assert(driver_dma_buffer_create(d,1,1,&b)==-ERANGE&&!b&&page_frees==2);
  buffer_destroy_owner(d);return;
 }
 if(!strcmp(name,"class-entry")) {
  driver_lifecycle_begin();pthread_t t;assert(!pthread_create(&t,NULL,buffer_create_worker,NULL));
  struct timespec until;assert(!clock_gettime(CLOCK_REALTIME,&until));until.tv_sec++;
  assert(!pthread_timedjoin_np(t,NULL,&until));driver_lifecycle_end();assert(!create_result&&thread_buffer);
  driver_dma_buffer_put(thread_buffer);buffer_destroy_owner(d);return;
 }
 memset(small_memory,0xa5,sizeof(small_memory));
 if(!strcmp(name,"maximum")) {
  chosen=(0x88800000ULL-pfa.ranges[0].base)/PAGE_SIZE;memset(large_memory,0xa5,sizeof(large_memory));
  assert(!driver_dma_buffer_create(d,sizeof(large_memory),sizeof(large_memory),&b));
  assert(driver_dma_buffer_address(b)==0x88800000&&selected_order==MAX_ORDER);
  assert(!large_memory[0]&&!large_memory[sizeof(large_memory)-1]);
  driver_dma_buffer_put(b);assert(page_frees==1&&freed_order==MAX_ORDER);buffer_destroy_owner(d);return;
 }
 d->dma_mask=0xffffffffULL;assert(!driver_dma_buffer_create(d,33,65536,&b));
 assert(limited_calls==1&&selected_order==4&&d->references==refs+1);
 assert(driver_dma_buffer_size(b)==33&&driver_dma_buffer_address(b)==0x88010000&&driver_dma_buffer_data(b)==small_memory);
 for(unsigned i=0;i<sizeof(small_memory);i++)assert(!small_memory[i]);
 driver_dma_buffer_get(NULL);driver_dma_buffer_put(NULL);
 assert(!driver_dma_buffer_data(NULL)&&!driver_dma_buffer_address(NULL)&&!driver_dma_buffer_size(NULL));
 if(!strcmp(name,"references")) {
  thread_buffer=b;pthread_t workers[8];
  for(unsigned i=0;i<8;i++)assert(!pthread_create(&workers[i],NULL,buffer_refs,NULL));
  for(unsigned i=0;i<8;i++)assert(!pthread_join(workers[i],NULL));
 } else assert(!strcmp(name,"lifetime"));
 driver_dma_buffer_get(b);assert(!device_unregister(d));device_put(d);assert(!owner_releases);
 small_memory[32]=0x7e;driver_dma_buffer_put(b);assert(!page_frees&&!owner_releases&&small_memory[32]==0x7e);
 driver_dma_buffer_put(b);assert(page_frees==1&&freed_order==4&&freed_pfn==5&&owner_releases==1);
}

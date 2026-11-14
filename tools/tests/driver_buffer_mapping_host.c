/* Production allocator + mapping + core; only PFA, cache and provider are modeled. */
static unsigned map_cpu_syncs, map_device_syncs;
void frame_get(pfn_t pfn) { (void)pfn; assert(!"buffer mapping must not pin order-N tails"); }
void frame_put(pfn_t pfn) { (void)pfn; assert(!"buffer mapping must not release order-N tails"); }
static void map_sync(void *data, size_t length) {
 assert(length==PAGE_SIZE && (char *)data>=(char *)small_memory);
 assert((char *)data<(char *)small_memory+sizeof(small_memory));
 assert(!(((char *)data-(char *)small_memory)&(PAGE_SIZE-1U)));
}
void dma_sync_for_cpu(void *data,size_t length) {map_sync(data,length);map_cpu_syncs++;}
void dma_sync_for_device(void *data,size_t length) {map_sync(data,length);map_device_syncs++;}

struct buffer_provider {
 uint64_t physical[16],addresses[16];
 unsigned calls,live,unmaps,releases,detaches,fail_map,fail_unmap;
 int error,pending,identity,bad_address,detach_error;
 enum driver_dma_direction direction;
 pthread_mutex_t lock;pthread_cond_t changed;
 int block,entered,proceed;
};
static int buffer_map_page(void *context,uint64_t physical,enum driver_dma_direction direction,
                           uint64_t *address,bool *needs_unmap) {
 struct buffer_provider *p=context;assert(direction==p->direction);unsigned index=p->calls++;
 assert(index<16);p->physical[index]=physical;
 if(p->block) {
  assert(!pthread_mutex_lock(&p->lock));p->entered=1;assert(!pthread_cond_broadcast(&p->changed));
  while(!p->proceed)assert(!pthread_cond_wait(&p->changed,&p->lock));
  assert(!pthread_mutex_unlock(&p->lock));
 }
 *needs_unmap=false;
 if(p->calls==p->fail_map&&!p->pending)return p->error;
 *address=p->identity?physical:0x10000ULL+index*2*PAGE_SIZE;
 if(p->bad_address)*address=0x10001;
 *needs_unmap=true;p->addresses[p->live++]=*address;
 return p->calls==p->fail_map?p->error:0;
}
static int buffer_unmap_page(void *context,uint64_t address,enum driver_dma_direction direction) {
 struct buffer_provider *p=context;assert(direction==p->direction&&p->live);
 assert(address==p->addresses[p->live-1]);
 if(++p->unmaps==p->fail_unmap)return -ETIMEDOUT;
 p->live--;return 0;
}
static int buffer_deactivate(void *context) {
 struct buffer_provider *p=context;assert(!p->live);p->detaches++;return p->detach_error;
}
static void buffer_provider_release(void *context) {
 struct buffer_provider *p=context;assert(!p->live);p->releases++;
}
static const struct driver_dma_managed_ops buffer_provider_ops={
 buffer_map_page,buffer_unmap_page,buffer_deactivate,buffer_provider_release
};
static struct driver_dma_buffer *map_test_buffer(size_t size) {
 pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0x8800b000,.end=0x8900b000,.start_pfn=0,.end_pfn=4096};
 struct driver_dma_buffer *b;device_t *owner=buffer_owner();
 assert(!driver_dma_buffer_create(owner,size,65536,&b)&&selected_order==4);return b;
}
static void buffer_finish_domain(struct driver_dma_domain *domain,struct buffer_provider *p,device_t *device) {
 assert(!driver_dma_domain_shutdown(domain));driver_dma_domain_put(domain);
 assert(p->detaches==1&&p->releases==1);buffer_destroy_owner(device);
}
static struct driver_dma_domain *thread_domain;
static struct driver_dma_buffer *map_thread_buffer;
static struct driver_dma_mapping *thread_mapping;
static int map_thread_result,close_thread_result,close_started,close_finished;
static void *buffer_map_thread(void *arg) {
 (void)arg;
 map_thread_result=driver_dma_map_buffer(thread_domain,map_thread_buffer,DRIVER_DMA_BIDIRECTIONAL,&thread_mapping);
 return NULL;
}
static void *buffer_close_thread(void *arg) {
 (void)arg;__atomic_store_n(&close_started,1,__ATOMIC_RELEASE);
 close_thread_result=driver_dma_domain_shutdown(thread_domain);
 __atomic_store_n(&close_finished,1,__ATOMIC_RELEASE);return NULL;
}

static struct driver_dma_domain *retiring_domain;
static unsigned owner_callbacks;
static void retiring_buffer_owner(device_t *owner) {
 assert(retiring_domain&&!retiring_domain->lock.depth&&!retiring_domain->mappings);
 assert(!driver_dma_domain_shutdown(retiring_domain));
 owner_callbacks++;buffer_owner_release(owner);
}

static void buffer_mapping_case(const char *name) {
 struct driver_dma_buffer *b=map_test_buffer(!strcmp(name,"padding")?33:8193);
 device_t *owner=allocation_owner,*target=buffer_owner();
 struct buffer_provider p={.direction=DRIVER_DMA_BIDIRECTIONAL,.error=-EIO};
 if(!strcmp(name,"to-device"))p.direction=DRIVER_DMA_TO_DEVICE;
 if(!strcmp(name,"from-device"))p.direction=DRIVER_DMA_FROM_DEVICE;
 struct driver_dma_domain *domain;struct driver_dma_mapping *m=(void *)1;
 uint64_t pa=driver_dma_buffer_address(b),address;
 struct driver_dma_policy *policy=NULL;
 if(!strcmp(name,"preflight")||!strcmp(name,"identity")||!strcmp(name,"excluded")) {
  bool identity=!strcmp(name,"identity"),excluded=!strcmp(name,"excluded");
  struct driver_dma_rule rule={identity?pa:excluded?pa+12288:pa+8193,excluded?pa+65535:pa+12287,
   identity?3:0,identity?DRIVER_DMA_POLICY_IDENTITY:0};
  assert(!driver_dma_policy_create(&rule,1,&policy));
  assert(!driver_dma_domain_create_managed_policy(target,&buffer_provider_ops,&p,policy,&domain));
  driver_dma_policy_put(policy);
 } else assert(!driver_dma_domain_create_managed(target,&buffer_provider_ops,&p,&domain));
 if(!strncmp(name,"reentrant-",10)) {
  retiring_domain=domain;owner->release=retiring_buffer_owner;
  if(!strcmp(name,"reentrant-rollback")) {
   p.fail_map=2;p.pending=1;p.fail_unmap=1;
   assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-EIO&&m&&p.live==2);
  }else{assert(!strcmp(name,"reentrant-normal"));assert(!driver_dma_map_buffer(domain,b,p.direction,&m));}
  driver_dma_buffer_put(b);b=NULL;buffer_destroy_owner(owner);owner=NULL;
  assert(!owner_callbacks&&!owner_releases&&!page_frees);
  assert(!driver_dma_try_unmap_pages(&m)&&!m&&owner_callbacks==1&&owner_releases==1&&page_frees==1);
  assert(p.detaches==1&&!p.live);
 }else if(!strcmp(name,"preflight")) {
  assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-EACCES&&!m&&!p.calls);
  assert(refcount_read(&b->references)==1&&!domain->mappings&&!map_device_syncs);
 } else if(!strcmp(name,"rollback")) {
  p.fail_map=2;p.pending=1;p.fail_unmap=1;
  assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-EIO&&m&&p.live==2);
  assert(driver_dma_mapping_count(m)==2&&refcount_read(&b->references)==2);
  assert(!map_device_syncs&&!map_cpu_syncs);
  assert(driver_dma_page_address(m,0,&address)==-ESHUTDOWN);
  assert(driver_dma_mapping_sync_for_cpu(m)==-ESHUTDOWN&&driver_dma_mapping_sync_for_device(m)==-ESHUTDOWN);
  assert(driver_dma_domain_shutdown(domain)==-EBUSY&&!p.detaches);
  driver_dma_buffer_put(b);b=NULL;buffer_destroy_owner(owner);owner=NULL;
  assert(!page_frees&&!owner_releases);small_memory[8192]=0xc9;
  assert(!driver_dma_try_unmap_pages(&m)&&!m&&page_frees==1&&owner_releases==1&&!map_cpu_syncs);
 } else if(!strcmp(name,"identity")) {
  p.fail_unmap=1;assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-ERANGE&&m&&p.live==1);
  assert(driver_dma_page_address(m,0,&address)==-ESHUTDOWN&&refcount_read(&b->references)==2);
  assert(!driver_dma_try_unmap_pages(&m)&&!m&&refcount_read(&b->references)==1);
  p.identity=1;assert(!driver_dma_map_buffer(domain,b,p.direction,&m));
  assert(!driver_dma_page_address(m,2,&address)&&address==pa+2*PAGE_SIZE);
  assert(!driver_dma_try_unmap_pages(&m)&&!m);
 } else if(!strcmp(name,"errors")) {
  assert(driver_dma_map_buffer(NULL,b,p.direction,&m)==-EINVAL&&!m);
  assert(driver_dma_map_buffer(domain,NULL,p.direction,&m)==-EINVAL&&!m);
  assert(driver_dma_map_buffer(domain,b,99,&m)==-EINVAL&&!m);
  assert(driver_dma_map_buffer(domain,b,p.direction,NULL)==-EINVAL);
  dma_ready=0;assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-EOPNOTSUPP&&!m);dma_ready=1;
  target->state=DEV_STATE_PREPARING;assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-ENODEV&&!m);target->state=DEV_STATE_UNINIT;
  fail_at=allocations+1;assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-ENOMEM&&!m);fail_at=0;
  p.fail_map=1;assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-EIO&&!m&&!p.live);
  assert(refcount_read(&b->references)==1&&!domain->mappings);
  p.fail_map=0;p.bad_address=1;
  assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-ERANGE&&!m&&!p.live);
  assert(!map_device_syncs&&!map_cpu_syncs);
 } else if(!strcmp(name,"shutdown")) {
  assert(!pthread_mutex_init(&p.lock,NULL)&&!pthread_cond_init(&p.changed,NULL));p.block=1;
  thread_domain=domain;map_thread_buffer=b;pthread_t mapper,closer;
  assert(!pthread_create(&mapper,NULL,buffer_map_thread,NULL));
  assert(!pthread_mutex_lock(&p.lock));while(!p.entered)assert(!pthread_cond_wait(&p.changed,&p.lock));
  assert(pthread_mutex_trylock(&domain->lock.lock)==EBUSY);
  assert(!pthread_create(&closer,NULL,buffer_close_thread,NULL));
  while(!__atomic_load_n(&close_started,__ATOMIC_ACQUIRE))sched_yield();
  assert(!__atomic_load_n(&close_finished,__ATOMIC_ACQUIRE));
  p.proceed=1;assert(!pthread_cond_broadcast(&p.changed));assert(!pthread_mutex_unlock(&p.lock));
  assert(!pthread_join(mapper,NULL)&&!pthread_join(closer,NULL));
  assert(!map_thread_result&&close_thread_result==-EBUSY&&thread_mapping&&!p.detaches);
  assert(driver_dma_map_buffer(domain,b,p.direction,&m)==-ESHUTDOWN&&!m);
  assert(!driver_dma_try_unmap_pages(&thread_mapping));
  assert(!pthread_cond_destroy(&p.changed)&&!pthread_mutex_destroy(&p.lock));
 } else {
  assert(!strcmp(name,"normal")||!strcmp(name,"padding")||!strcmp(name,"unmap")||
         !strcmp(name,"to-device")||!strcmp(name,"from-device")||!strcmp(name,"excluded"));
  assert(!driver_dma_map_buffer(domain,b,p.direction,&m));
  unsigned count=!strcmp(name,"padding")?1:3;assert(driver_dma_mapping_count(m)==count&&p.calls==count);
  for(unsigned i=0;i<count;i++) {
   assert(p.physical[i]==pa+i*PAGE_SIZE);
   assert(!driver_dma_page_address(m,i,&address)&&address==0x10000ULL+i*2*PAGE_SIZE);
  }
  assert(map_device_syncs==count&&!map_cpu_syncs&&refcount_read(&b->references)==2);
  driver_dma_buffer_put(b);b=NULL;buffer_destroy_owner(owner);owner=NULL;
  assert(!owner_releases&&!page_frees);small_memory[(count-1)*PAGE_SIZE]=0x5a;
  if(!strcmp(name,"unmap")) {
   p.fail_unmap=2;assert(driver_dma_try_unmap_pages(&m)==-ETIMEDOUT&&m&&p.live==2);
   assert(!map_cpu_syncs&&!page_frees&&!owner_releases&&driver_dma_mapping_count(m)==3);
   assert(driver_dma_page_address(m,0,&address)==-ESHUTDOWN);
   assert(driver_dma_domain_shutdown(domain)==-EBUSY&&!p.detaches);
  } else {
   assert(!driver_dma_mapping_sync_for_cpu(m)&&map_cpu_syncs==(p.direction==DRIVER_DMA_TO_DEVICE?0:count));
   map_cpu_syncs=0;
  }
  assert(!driver_dma_try_unmap_pages(&m)&&!m&&page_frees==1&&owner_releases==1);
  assert(map_cpu_syncs==(p.direction==DRIVER_DMA_TO_DEVICE?0:count)&&!p.live&&freed_order==4&&freed_pfn==5);
 }
 if(b)driver_dma_buffer_put(b);
 if(owner)buffer_destroy_owner(owner);
 buffer_finish_domain(domain,&p,target);
 assert(page_frees==1&&owner_releases==2);
}

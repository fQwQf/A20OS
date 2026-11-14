"""Resident factory/cgroup/Rust lifetime plus production DMA ownership and rollback."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_vmo_contiguous import build_fixture, ROOT
from .test_vmo_resident import model as resident_model

EXTRA=r'''
#include "drivers/core/driver_core.h"
#include "drivers/core/dma_mapping.h"
#include "drivers/core/dma_buffer.h"
_Thread_local unsigned dma_lock_depth;
static int dma_ready=1;
static unsigned cpu_syncs,device_syncs,owner_events;
static struct driver_dma_domain *observed_domain;
static cg_node_t *budget;
static bool close_on_destroy,provider_must_follow_owner;
device_t *device_get(device_t *d){assert(d&&d->references);d->references++;return d;}
void device_put(device_t *d){assert(d&&d->references);d->references--;}
uint64_t dma_get_mask(const device_t *d){return d->dma_mask?d->dma_mask:~(uint64_t)0;}
static void cache_valid(void *data){uintptr_t p=(uintptr_t)data;assert(p>=host_page_offset&&p-host_page_offset<pfa.total_frames*PAGE_SIZE);pfn_t page=(p-host_page_offset)/PAGE_SIZE;assert(pfn_valid(page)&&pfa.meta[page].refcount);}
void dma_sync_for_device(void *p,size_t n){assert(n==PAGE_SIZE);cache_valid(p);device_syncs++;}
void dma_sync_for_cpu(void *p,size_t n){assert(n==PAGE_SIZE);cache_valid(p);cpu_syncs++;}
void driver_dma_buffer_get(struct driver_dma_buffer *b){assert(!b);}
void driver_dma_buffer_put(struct driver_dma_buffer *b){assert(!b);}
void *driver_dma_buffer_data(const struct driver_dma_buffer *b){(void)b;assert(0);return NULL;}
uint64_t driver_dma_buffer_address(const struct driver_dma_buffer *b){(void)b;assert(0);return 0;}
size_t driver_dma_buffer_size(const struct driver_dma_buffer *b){(void)b;assert(0);return 0;}
#include "drivers/core/dma_policy.c"
#include "drivers/core/dma_mapping.c"
#include "drivers/core/dma_vmo.c"
int arch_device_dma_ready(void){return dma_ready;}
struct provider {bool managed,uncertain,bad_address;unsigned calls,live,unmaps,releases,detaches,fail_map,fail_unmap;int detach_error;uint64_t addresses[16];pfn_t expected[4];unsigned expected_count;};
static int provider_map(void *context,uint64_t physical,enum driver_dma_direction dir,uint64_t *address,bool *needs_unmap){
 struct provider *p=context;assert(dma_lock_depth==(p->managed?1u:0u)&&dir==DRIVER_DMA_BIDIRECTIONAL);
 pfn_t page=phys_to_pfn(physical);assert(pfn_valid(page)&&pfa.meta[page].refcount>=2);assert(p->calls<p->expected_count&&page==p->expected[p->calls]);p->calls++;*needs_unmap=false;
 if(p->calls==p->fail_map&&!p->uncertain)return -EIO;
 *address=0x200000+p->calls*PAGE_SIZE+(p->bad_address?1:0);assert(p->calls<16);p->addresses[p->calls]=*address;p->live++;*needs_unmap=true;
 return p->calls==p->fail_map?-EIO:0;
}
static int checked_map(void *c,uint64_t p,enum driver_dma_direction d,uint64_t *a){bool needs;return provider_map(c,p,d,a,&needs);}
static int provider_unmap(void *context,uint64_t address,enum driver_dma_direction dir){
 struct provider *p=context;assert(dma_lock_depth==(p->managed?1u:0u)&&dir==DRIVER_DMA_BIDIRECTIONAL);
 unsigned slot=1;while(slot<=p->calls&&p->addresses[slot]!=address)slot++;assert(slot<=p->calls&&p->live);
 if(++p->unmaps==p->fail_unmap)return -ETIMEDOUT;p->addresses[slot]=0;p->live--;return 0;
}
static int provider_deactivate(void *context){struct provider *p=context;assert(dma_lock_depth==1&&!p->live);p->detaches++;return p->detach_error;}
static void provider_release(void *context){struct provider *p=context;assert(!dma_lock_depth&&!p->live);if(provider_must_follow_owner)assert(owner_events==1&&!budget->res.mem.rss);p->releases++;}
static const struct driver_dma_managed_ops managed_ops={provider_map,provider_unmap,provider_deactivate,provider_release};
static const struct driver_dma_checked_ops checked_ops={checked_map,provider_unmap,provider_release};
static void owner_destroy(void *object){
 struct vmo *v=object;assert(!dma_lock_depth);
 if(v->type!=VMO_RESIDENT)return;
 assert(v->charge_cg==(struct cg_node *)budget&&budget->res.mem.rss==4&&budget->namespace_refs);owner_events++;
 if(close_on_destroy){assert(observed_domain&&!observed_domain->mappings);assert(!driver_dma_domain_shutdown(observed_domain));}
}
'''

MAIN=r'''
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];size_t capacity=64;
 unsigned char *ram=aligned_alloc(PAGE_SIZE,capacity*PAGE_SIZE);assert(ram);memset(ram,0xa5,capacity*PAGE_SIZE);
 host_page_offset=(uintptr_t)ram;pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0,.end=capacity*PAGE_SIZE,.start_pfn=0,.end_pfn=capacity};pfa.total_frames=capacity;pfa.meta=calloc(capacity,sizeof(*pfa.meta));assert(pfa.meta);spin_init(&pfa.lock);
 for(pfn_t p=1;p<32;p+=2)pfa.meta[p]=(frame_meta_t){.flags=FRAME_F_ALLOC,.refcount=1};
 cg_node_t cg={0};spin_init(&cg.lock);cg_mem_init(&cg.res.mem);cg.namespace_refs=1;cg.res.mem.limit=4;budget=&cg;
 struct vmo *memory=NULL;assert(!vmo_create_resident(3*PAGE_SIZE+33,&cg,&memory));assert(memory->pages[1]+1!=memory->pages[2]);host_destroy_hook=owner_destroy;
 device_t device={.name="resident-map-host",.references=1,.state=DEV_STATE_RUNNING,.dma_mask=~(uint64_t)0};
 struct provider provider={.managed=strcmp(name,"checked")!=0,.expected={memory->pages[1],memory->pages[2]},.expected_count=2};struct driver_dma_domain *domain=NULL;
 if(!strcmp(name,"policy")){
  uint64_t pa=pfn_to_phys(memory->pages[1]);struct driver_dma_rule rule={pa,pa+PAGE_SIZE-1,0,0};struct driver_dma_policy *policy;
  assert(!driver_dma_policy_create(&rule,1,&policy));assert(!driver_dma_domain_create_managed_policy(&device,&managed_ops,&provider,policy,&domain));driver_dma_policy_put(policy);
 }else if(provider.managed)assert(!driver_dma_domain_create_managed(&device,&managed_ops,&provider,&domain));
 else assert(!driver_dma_domain_create_checked(&device,&checked_ops,&provider,&domain));
 observed_domain=domain;struct driver_dma_mapping *mapping=(void *)1;size_t before=heap_live;
 if(!strcmp(name,"parameters")){
  assert(driver_dma_map_owned_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,NULL)==-EINVAL);
  assert(driver_dma_map_owned_vmo(NULL,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_owned_vmo(domain,NULL,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_owned_vmo(domain,memory,1,0,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_owned_vmo(domain,memory,1,2,(enum driver_dma_direction)99,&mapping)==-EINVAL&&!mapping);
  assert(driver_dma_map_owned_vmo(domain,memory,0,VMO_PIN_MAX_PAGES+1,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-E2BIG&&!mapping);
  assert(driver_dma_map_owned_vmo(domain,memory,(uint32_t)-1,1,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-EINVAL&&!mapping);
 }else if(!strcmp(name,"kinds")){
  for(unsigned type=0;type<=2;type++){
   struct vmo *other=vmo_create(type,PAGE_SIZE,0);assert(other);
   assert(driver_dma_map_owned_vmo(domain,other,0,1,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-EOPNOTSUPP&&!mapping);vmo_release(other);
  }
  struct vmo *other;assert(!vmo_create_contiguous(PAGE_SIZE,&other));
  pfn_t page=vmo_peek_page(other,0);provider.expected[0]=page;provider.expected_count=1;
  assert(!driver_dma_map_owned_vmo(domain,other,0,1,DRIVER_DMA_BIDIRECTIONAL,&mapping)&&mapping&&refcount_read(&other->refcount)==2);
  vmo_release(other);assert(vmo_peek_page(other,0)==page&&pfa.meta[page].refcount==2);
  assert(!driver_dma_try_unmap_pages(&mapping)&&!mapping&&allocated_pages==4&&cg.res.mem.rss==4);
 }else if(!strcmp(name,"oom")){
  for(unsigned at=1;at<=2;at++){fail_at=attempts+at;assert(driver_dma_map_owned_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-ENOMEM&&!mapping);fail_at=0;assert(heap_live==before&&refcount_read(&memory->refcount)==1&&cg.res.mem.rss==4);}
 }else if(!strcmp(name,"preflight")){
  dma_ready=0;assert(driver_dma_map_owned_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-EOPNOTSUPP&&!mapping);dma_ready=1;
  device.state=DEV_STATE_REMOVING;assert(driver_dma_map_owned_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-ENODEV&&!mapping);device.state=DEV_STATE_RUNNING;
  assert(!driver_dma_domain_shutdown(domain));assert(driver_dma_map_owned_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-ESHUTDOWN&&!mapping);
 }else if(!strcmp(name,"policy")){
  assert(driver_dma_map_owned_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping)==-EACCES&&!mapping&&!provider.calls);
 }else{
  bool legacy=!strcmp(name,"snapshot");
  bool failed=!strcmp(name,"rollback-retained")||!strcmp(name,"rollback-complete")||!strcmp(name,"bad-address");
  if(failed){provider.fail_map=2;provider.uncertain=strcmp(name,"rollback-complete")!=0;if(provider.uncertain)provider.fail_unmap=1;}
  if(!strcmp(name,"bad-address")){provider.fail_map=0;provider.bad_address=true;}
  int error=failed?(!strcmp(name,"bad-address")?-ERANGE:-EIO):0;
  int result=legacy?driver_dma_map_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping):driver_dma_map_owned_vmo(domain,memory,1,2,DRIVER_DMA_BIDIRECTIONAL,&mapping);
  assert(result==error);
  if(!strcmp(name,"rollback-complete")){assert(!mapping&&!provider.live&&refcount_read(&memory->refcount)==1&&cg.res.mem.rss==4);}
  else{
   assert(mapping&&driver_dma_mapping_count(mapping)==(!strcmp(name,"bad-address")?1u:2u));
   assert(refcount_read(&memory->refcount)==(legacy?1:2)&&cg.res.mem.rss==4&&cg.namespace_refs==2);
   struct driver_dma_mapping *second=NULL;
   if(!strcmp(name,"views")){provider.expected[2]=memory->pages[0];provider.expected_count=3;assert(!driver_dma_map_owned_vmo(domain,memory,0,1,DRIVER_DMA_BIDIRECTIONAL,&second));assert(refcount_read(&memory->refcount)==3&&cg.res.mem.rss==4);}
   vmo_release(memory);memory=NULL;
   if(legacy){assert(owner_events==1&&!cg.res.mem.rss&&allocated_pages==2);}
   else{cg_mem_uncharge(&cg,SIZE_MAX);assert(cg.res.mem.rss==4&&allocated_pages==4&&!owner_events);}
   if(!strcmp(name,"unmap")||!strcmp(name,"checked")){
    provider.fail_unmap=2;assert(driver_dma_try_unmap_pages(&mapping)==-ETIMEDOUT&&mapping&&cg.res.mem.rss==4&&allocated_pages==4);
    assert(driver_dma_mapping_sync_for_device(mapping)==-ESHUTDOWN&&driver_dma_mapping_sync_for_cpu(mapping)==-ESHUTDOWN);
    uint64_t address;assert(driver_dma_page_address(mapping,0,&address)==-ESHUTDOWN);
    if(provider.managed)assert(driver_dma_domain_shutdown(domain)==-EBUSY);
   }
   if(failed){assert(mapping&&driver_dma_mapping_sync_for_device(mapping)==-ESHUTDOWN&&cg.res.mem.rss==4&&!owner_events);}
   if(!strcmp(name,"reentrant")){
    close_on_destroy=true;provider_must_follow_owner=true;driver_dma_domain_put(domain);
   }
   provider.fail_unmap=0;assert(!driver_dma_try_unmap_pages(&mapping)&&!mapping);
   if(second){assert(cg.res.mem.rss==4&&allocated_pages==4&&!owner_events);assert(!driver_dma_try_unmap_pages(&second));}
   assert(!cg.res.mem.rss&&!cg.res.mem.shared_anon_rss&&cg.namespace_refs==1&&!allocated_pages&&owner_events==1);
   if(!strcmp(name,"reentrant")){assert(provider.releases==1);domain=NULL;}
   if(!strcmp(name,"detach")){provider.detach_error=-ETIMEDOUT;assert(driver_dma_domain_shutdown(domain)==-ETIMEDOUT&&!provider.releases&&!cg.res.mem.rss);provider.detach_error=0;}
  }
 }
 if(memory){assert(refcount_read(&memory->refcount)==1&&!provider.live&&cg.res.mem.rss==4);vmo_release(memory);}
 if(domain){if(provider.managed)assert(!driver_dma_domain_shutdown(domain));driver_dma_domain_put(domain);}
 assert(provider.releases==1&&device.references==1&&!cg.res.mem.rss&&!cg.res.mem.shared_anon_rss&&cg.namespace_refs==1);
 assert(!heap_live&&!allocated_pages&&!g_a20_objstats.vmos&&!g_a20_objstats.vmo_pages&&!destroying&&!dma_lock_depth);
 free(pfa.meta);free(ram);puts("DRIVER_RESIDENT_MAP: PASS");return 0;
}
'''


def build_model(source,work):
    source=resident_model(source)
    sync=work/'core/sync.h'
    sync.write_text(sync.read_text()+r'''
#include <assert.h>
#include <pthread.h>
typedef pthread_mutex_t mutex_t;
extern _Thread_local unsigned dma_lock_depth;
static inline void mutex_init(mutex_t *m){assert(!pthread_mutex_init(m,NULL));}
static inline void mutex_lock(mutex_t *m){assert(!pthread_mutex_lock(m));dma_lock_depth++;}
static inline void mutex_unlock(mutex_t *m){assert(dma_lock_depth);dma_lock_depth--;assert(!pthread_mutex_unlock(m));}
''')
    hw=work/'drivers/core/driver_hwapi.h';hw.parent.mkdir(parents=True,exist_ok=True)
    hw.write_text('#pragma once\n#include "core/types.h"\nstruct device;\nuint64_t dma_get_mask(const struct device *);\nvoid dma_sync_for_device(void *,size_t);\nvoid dma_sync_for_cpu(void *,size_t);\n')
    return source


class DriverResidentMapping(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-resident-map-');cls.addClassCleanup(cls.temp.cleanup)
        work=Path(cls.temp.name)
        cls.binary=build_fixture(work,suffix=EXTRA+MAIN,extra_vmo=['int vmo_create_resident('],transform=lambda source:build_model(source,work))
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_invalid_arguments_ranges_and_limits_leave_no_owner(self):self.run_case('parameters')
    def test_factory_fixed_ram_includes_contiguous_and_resident_sources(self):self.run_case('kinds')
    def test_allocation_failures_return_snapshot_and_object_references(self):self.run_case('oom')
    def test_closed_offline_and_architecture_failures_preserve_caller_budget(self):self.run_case('preflight')
    def test_policy_rejection_calls_no_provider_and_takes_no_owner(self):self.run_case('policy')
    def test_subset_mapping_holds_the_whole_extent_and_original_charge(self):self.run_case('normal')
    def test_multiple_views_share_one_charge_until_the_last_unmap(self):self.run_case('views')
    def test_partial_unmap_keeps_all_storage_and_budget_until_retry(self):self.run_case('unmap')
    def test_uncertain_map_failure_returns_an_owning_cleanup_handle(self):self.run_case('rollback-retained')
    def test_confirmed_map_rollback_installs_no_owner(self):self.run_case('rollback-complete')
    def test_invalid_iova_retains_the_whole_owner_during_failed_cleanup(self):self.run_case('bad-address')
    def test_checked_legacy_provider_also_preserves_owner_on_failed_unmap(self):self.run_case('checked')
    def test_original_snapshot_mapper_retains_frames_without_a_vmo_charge(self):self.run_case('snapshot')
    def test_final_owner_put_can_reenter_domain_shutdown_outside_its_lock(self):self.run_case('reentrant')
    def test_domain_detach_failure_does_not_hold_already_unmapped_backing(self):self.run_case('detach')

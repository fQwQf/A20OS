"""Production resident factory, Rust lifetime/destruction and real cgroup budgets."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from .test_vmo_contiguous import build_fixture, ROOT, extract_function


def model(source):
    cg=(ROOT/'kernel/include/cg/cgroup.h').read_text()
    typ=cg[cg.index('typedef struct cg_mem_state {'):cg.index('} cg_mem_state_t;')+len('} cg_mem_state_t;')]
    prefix=typ+r'''
typedef struct cg_node {spinlock_t lock;struct {cg_mem_state_t mem;} res;unsigned namespace_refs;} cg_node_t;
static int spin_trylock_irqsave(spinlock_t *l,uint64_t *f){*f=0;return !pthread_mutex_trylock(&l->native);}
#define A20_PERF_MM_CG_LOCK_CONTENDED 0
#define A20_PERF_MM_CG_LOCK_ACQUIRES 1
static void a20_perf_count(int n){(void)n;}
static unsigned pfa_attempts,pfa_fail_at,alias_fail_at;
'''
    source=source.replace('#include "core/errno.h"','#include "core/errno.h"\n'+prefix)
    source=source.replace('if(pfa_oom)return PFN_NONE;', 'assert(order==0);if(++pfa_attempts==pfa_fail_at||pfa_oom)return PFN_NONE;')
    source=source.replace('allocated_pages+=n;return h;', 'allocated_pages+=n;if(pfa_attempts==alias_fail_at)host_page_offset=0-(uintptr_t)h*PAGE_SIZE;return h;')
    for signature in ('static void cg_mem_uncharge_shared_anon(', 'static void cg_mem_uncharge(', 'static void cg_group_unpin('):
        source=source.replace(extract_function(source,signature),'')
    cgsource=(ROOT/'kernel/mm/cg_mem.c').read_text()
    signatures=['void cg_mem_init(', 'static int cg_mem_charge_kind(', 'int cg_mem_charge(', 'int cg_mem_charge_shared_anon(',
                'void cg_mem_uncharge(', 'void cg_mem_uncharge_shared_anon(', 'void cg_group_pin(', 'void cg_group_unpin(']
    functions='\n'.join(extract_function(cgsource,n) for n in signatures)
    return source.replace('/* PRODUCTION_VMO */','pfn_t pfa_alloc_page(void){return pfa_alloc(0);}'+'\n'+functions+'\n/* PRODUCTION_VMO */')


MAIN=r'''
static void *refs(void *p){for(unsigned i=0;i<10000;i++){vmo_ref(p);vmo_release(p);}return NULL;}
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];
 size_t capacity=((size_t)1<<MAX_ORDER)+32;
 unsigned char *ram=aligned_alloc(PAGE_SIZE,capacity*PAGE_SIZE);assert(ram);memset(ram,0xa5,capacity*PAGE_SIZE);
 host_page_offset=(uintptr_t)ram;pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0,.end=capacity*PAGE_SIZE,.start_pfn=0,.end_pfn=capacity};
 pfa.total_frames=capacity;pfa.meta=calloc(capacity,sizeof(*pfa.meta));assert(pfa.meta);spin_init(&pfa.lock);
 cg_node_t cg={0};spin_init(&cg.lock);cg_mem_init(&cg.res.mem);cg.namespace_refs=1;
 struct vmo *v=(void *)1;
 if(!strcmp(name,"parameters")){
  assert(vmo_create_resident(PAGE_SIZE,&cg,NULL)==-EINVAL);
  assert(vmo_create_resident(0,&cg,&v)==-EINVAL&&!v);
  assert(vmo_create_resident(SIZE_MAX,&cg,&v)==-E2BIG&&!v);
  assert(!vmo_create(VMO_RESIDENT,PAGE_SIZE,0));
  if(SIZE_MAX>0xffffffffU)assert(vmo_create_resident(((uint64_t)0xffffffffU+1)*PAGE_SIZE,&cg,&v)==-E2BIG&&!v);
 }else if(!strcmp(name,"metadata-oom")){
  for(unsigned at=1;at<=2;at++){fail_at=attempts+at;assert(vmo_create_resident(3*PAGE_SIZE,&cg,&v)==-ENOMEM&&!v&&!heap_live&&!allocated_pages&&!cg.res.mem.rss&&cg.namespace_refs==1);fail_at=0;}
 }else if(!strcmp(name,"page-oom")||!strcmp(name,"alias-failure")){
  for(unsigned at=1;at<=4;at++){
   pfa_attempts=0;if(!strcmp(name,"page-oom"))pfa_fail_at=at;else alias_fail_at=at;
   assert(vmo_create_resident(3*PAGE_SIZE+1,&cg,&v)==(pfa_fail_at?-ENOMEM:-ERANGE)&&!v);
   host_page_offset=(uintptr_t)ram;
   assert(!allocated_pages&&!heap_live&&!cg.res.mem.rss&&cg.namespace_refs==1&&!g_a20_objstats.vmos&&!g_a20_objstats.vmo_pages);
  }
 }else if(!strcmp(name,"charge-failure")){
  cg.res.mem.limit=2;size_t before=attempts;
  assert(vmo_create_resident(2*PAGE_SIZE+1,&cg,&v)==-ENOMEM&&!v&&!pfa_attempts&&attempts==before&&cg.res.mem.failcnt==1);
 }else{
  size_t bytes=3*PAGE_SIZE+33;
  if(!strcmp(name,"fragmentation"))for(pfn_t i=1;i<32;i+=2)pfa.meta[i]=(frame_meta_t){.flags=FRAME_F_ALLOC,.refcount=1};
  if(!strcmp(name,"larger-than-order"))bytes=((size_t)PAGE_SIZE<<MAX_ORDER)+1;
  struct cg_node *owner=!strcmp(name,"kernel-owned")?NULL:&cg;
  assert(!vmo_create_resident(bytes,owner,&v)&&v&&v->type==VMO_RESIDENT);
  size_t rounded=(bytes+PAGE_SIZE-1)&~(size_t)(PAGE_SIZE-1),count=rounded/PAGE_SIZE;
  assert(v->size==rounded&&v->phys_size==rounded&&v->page_count==count&&allocated_pages==count);
  assert(cg.res.mem.rss==(owner?count:0)&&cg.res.mem.shared_anon_rss==cg.res.mem.rss&&cg.namespace_refs==(owner?2u:1u));
  for(uint32_t i=0;i<v->page_count;i++){
   pfn_t page=vmo_peek_page(v,i);assert(pfn_valid(page)&&pfa.meta[page].order==0&&pfa.meta[page].refcount==1);
   if(!strcmp(name,"fragmentation"))assert(page==2*i);
   unsigned char *data=pfn_to_virt(page);for(size_t n=0;n<PAGE_SIZE;n++)assert(!data[n]);data[0]=(unsigned char)(i+1);
  }
  uint64_t pa=9;size_t span=9;assert(vmo_contiguous_info(v,&pa,&span)==-EINVAL&&!pa&&!span);
  if(!strcmp(name,"immutable")){
   uint64_t flags=88;assert(vmo_resize(v,0)==-EOPNOTSUPP&&vmo_resize(v,rounded)==-EOPNOTSUPP&&vmo_resize(v,rounded+PAGE_SIZE)==-EOPNOTSUPP);
   assert(a20_vmo_resize_lock(v,&flags)==-EOPNOTSUPP&&flags==88);
   pfn_t saved=v->pages[1];v->pages[1]=PFN_NONE;assert(a20_vmo_page_lock(v,1,&flags)==-EIO&&flags==88);v->pages[1]=saved;
  }else if(!strcmp(name,"budget-lifetime")){
   cg_mem_uncharge(&cg,SIZE_MAX);assert(cg.res.mem.rss==count);
   vmo_ref(v);vmo_release(v);assert(cg.res.mem.rss==count&&cg.namespace_refs==2);
   cg_group_unpin(&cg);assert(cg.namespace_refs==1);vmo_release(v);v=NULL;
   assert(!cg.namespace_refs&&!cg.res.mem.rss&&!cg.res.mem.shared_anon_rss);cg_group_pin(&cg);
  }else if(!strcmp(name,"pins")){
   pfn_t pages[2];assert(!vmo_pin_pages(v,1,2,pages));assert(pfa.meta[pages[0]].refcount==2);
   vmo_release(v);v=NULL;assert(allocated_pages==2&&!cg.res.mem.rss&&cg.namespace_refs==1);
   assert(((unsigned char*)pfn_to_virt(pages[0]))[0]==2);frame_put_many(pages,2);
  }else if(!strcmp(name,"concurrent")){
   pthread_t threads[8];for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,refs,v));
   for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
  }else assert(!strcmp(name,"fragmentation")||!strcmp(name,"larger-than-order")||!strcmp(name,"kernel-owned"));
  if(v)vmo_release(v);
 }
 assert(!allocated_pages&&!heap_live&&!raw_frees&&!g_a20_objstats.vmos&&!g_a20_objstats.vmo_pages&&!destroying);
 assert(!cg.res.mem.rss&&!cg.res.mem.shared_anon_rss&&cg.namespace_refs==1);
 free(pfa.meta);free(ram);puts("VMO_RESIDENT: PASS");return 0;
}
'''


class VmoResident(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp=tempfile.TemporaryDirectory(prefix='a20-resident-vmo-');cls.addClassCleanup(cls.temp.cleanup)
        cls.binary=build_fixture(Path(cls.temp.name),suffix=MAIN,extra_vmo=['int vmo_create_resident('],transform=model)
    def run_case(self,name):subprocess.run([str(self.binary),name],check=True,timeout=15)
    def test_factory_only_kind_and_representable_extents(self):self.run_case('parameters')
    def test_fragmented_order_zero_pages_and_zeroed_padding(self):self.run_case('fragmentation')
    def test_extent_larger_than_the_largest_buddy_allocation(self):self.run_case('larger-than-order')
    def test_metadata_allocation_failure_returns_charge_and_group_pin(self):self.run_case('metadata-oom')
    def test_each_partial_page_allocation_failure_rolls_back(self):self.run_case('page-oom')
    def test_each_invalid_cpu_alias_rolls_back(self):self.run_case('alias-failure')
    def test_budget_rejection_precedes_allocation(self):self.run_case('charge-failure')
    def test_object_charge_survives_task_rss_teardown_and_owner_exit(self):self.run_case('budget-lifetime')
    def test_extent_and_slots_cannot_be_resized_or_rematerialized(self):self.run_case('immutable')
    def test_independent_page_pins_preserve_storage_without_transferring_charge(self):self.run_case('pins')
    def test_kernel_owned_storage_explicitly_has_no_group_charge(self):self.run_case('kernel-owned')
    def test_concurrent_references_release_every_page_and_charge_once(self):self.run_case('concurrent')

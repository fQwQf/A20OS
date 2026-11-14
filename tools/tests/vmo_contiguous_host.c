/* Production contiguous VMO factory, PFA ownership split, Rust lifecycle and
 * destructor. PFA allocation/free, CPU aliases and notification are modeled. */
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <pthread.h>
#include "mm/vmo.h"
#include "core/errno.h"
uintptr_t host_page_offset;
pfa_t pfa;
static size_t heap_live,attempts,fail_at,allocated_pages,raw_frees,canonical_puts,flushes;
static int pfa_oom,split_error;
static struct vmo *destroying;
static size_t destroy_barrier;
static int destroy_needs_barrier;
static void (*host_destroy_hook)(void *);
struct {long vmos,vmo_pages;} g_a20_objstats;
static void a20_objstat_add(long *counter,int64_t n){*counter+=n;assert(*counter>=0);}
void *kmalloc(size_t n){if(++attempts==fail_at)return NULL;void *p=malloc(n);assert(p);heap_live++;return p;}
void *kmalloc_atomic(size_t n){return kmalloc(n);}
void kfree(void *p){if(p){if(p==destroying)destroying=NULL;assert(heap_live);heap_live--;free(p);}}
void panic(const char *fmt,...){(void)fmt;abort();}
pfn_t pfa_alloc(int order){
 if(pfa_oom)return PFN_NONE;assert(order>=0&&order<=MAX_ORDER);pfn_t n=1u<<order;
 for(pfn_t h=0;h+n<=pfa.total_frames;h+=n){int free=1;for(pfn_t i=0;i<n;i++)if(pfa.meta[h+i].flags)free=0;
  if(!free)continue;for(pfn_t i=0;i<n;i++)pfa.meta[h+i]=(frame_meta_t){.flags=FRAME_F_ALLOC};
  pfa.meta[h].order=order;pfa.meta[h].refcount=1;allocated_pages+=n;return h;
 }return PFN_NONE;
}
void pfa_free(pfn_t h,int order){
 assert(pfn_valid(h)&&pfa.meta[h].order==order&&pfa.meta[h].refcount==1);pfn_t n=1u<<order;
 for(pfn_t i=1;i<n;i++)assert(!pfa.meta[h+i].refcount);
 for(pfn_t i=0;i<n;i++)memset(&pfa.meta[h+i],0,sizeof(pfa.meta[h+i]));
 allocated_pages-=n;raw_frees++;
}
void frame_get(pfn_t p){assert(pfn_valid(p)&&pfa.meta[p].order==0&&pfa.meta[p].refcount);pfa.meta[p].refcount++;}
void frame_put(pfn_t p){
 assert(pfn_valid(p)&&pfa.meta[p].order==0&&pfa.meta[p].refcount);
 if(destroying&&destroy_needs_barrier)assert(flushes>destroy_barrier);
 canonical_puts++;if(!--pfa.meta[p].refcount){pfa.meta[p].flags=0;allocated_pages--;}
}
void frame_get_many(const pfn_t *p,size_t n){for(size_t i=0;i<n;i++)frame_get(p[i]);}
void frame_put_many(const pfn_t *p,size_t n){for(size_t i=0;i<n;i++)frame_put(p[i]);}
#define pfa_split_allocated production_split_allocated
/* PRODUCTION_SPLIT */
#undef pfa_split_allocated
int pfa_split_allocated(pfn_t p,int order){return split_error?-1:production_split_allocated(p,order);}
struct vmo_charge {struct vmo_charge *next;struct cg_node *cg;uint64_t pages;};
#define A20_OBJ_MEMORY 5
static void a20_eventq_on_object_destroy(void *obj,int kind){assert(kind==5);destroying=obj;destroy_barrier=flushes;destroy_needs_barrier=((struct vmo *)obj)->phys_size!=0;if(host_destroy_hook)host_destroy_hook(obj);}
static void a20_pager_detach_vmo(struct vmo *v){(void)v;assert(0);}
static void cg_mem_uncharge_shared_anon(struct cg_node *c,uint64_t n){(void)c;(void)n;assert(0);}
static void cg_mem_uncharge(struct cg_node *c,uint64_t n){(void)c;(void)n;assert(0);}
static void cg_group_unpin(struct cg_node *c){(void)c;assert(0);}
static void a20_vmo_sparse_destroy(void *r,struct vmo *v){(void)r;(void)v;assert(0);}
pfn_t a20_vmo_sparse_peek(void *r,uint32_t i){(void)r;(void)i;assert(0);return PFN_NONE;}
static void arch_tlb_flush(void){flushes++;}
extern void a20_vmo_ref_rust(struct vmo *);
extern void a20_vmo_release_rust(struct vmo *);
extern pfn_t a20_vmo_peek_page_rust(struct vmo *,uint32_t);
extern int a20_vmo_pin_pages_rust(struct vmo *,uint32_t,uint32_t,pfn_t *);
int64_t a20_vmo_resize_rust(struct vmo *v,uint64_t n){(void)v;(void)n;assert(0);return -EINVAL;}
/* PRODUCTION_VMO */
static void *ref_worker(void *p){for(unsigned i=0;i<10000;i++){vmo_ref(p);vmo_release(p);}return NULL;}
int main(int argc,char **argv){
 assert(argc==2);const char *name=argv[1];size_t capacity=(size_t)1<<MAX_ORDER;
 unsigned char *ram=aligned_alloc(PAGE_SIZE,capacity*PAGE_SIZE);assert(ram);memset(ram,0xa5,capacity*PAGE_SIZE);
 host_page_offset=(uintptr_t)ram;pfa.nr_ranges=1;pfa.ranges[0]=(pfa_range_t){.base=0,.end=capacity*PAGE_SIZE,.start_pfn=0,.end_pfn=capacity};
 pfa.total_frames=capacity;pfa.meta=calloc(capacity,sizeof(*pfa.meta));assert(pfa.meta);spin_init(&pfa.lock);
 struct vmo *v=(void *)1;
 if(!strcmp(name,"parameters")){
  assert(vmo_create_contiguous(PAGE_SIZE,NULL)==-EINVAL);
  assert(vmo_create_contiguous(0,&v)==-EINVAL&&!v);
  assert(vmo_create_contiguous(((size_t)PAGE_SIZE<<MAX_ORDER)+1,&v)==-E2BIG&&!v);
  assert(!vmo_create(VMO_CONTIGUOUS,PAGE_SIZE,0));
  uint64_t pa=9;size_t bytes=9;assert(vmo_contiguous_info(NULL,&pa,&bytes)==-EINVAL&&!pa&&!bytes);
  struct vmo other={0};pa=bytes=9;assert(vmo_contiguous_info(&other,&pa,&bytes)==-EINVAL&&!pa&&!bytes);
 }else if(!strcmp(name,"oom")){
  for(unsigned i=1;i<=2;i++){fail_at=attempts+i;assert(vmo_create_contiguous(3*PAGE_SIZE,&v)==-ENOMEM&&!v&&!allocated_pages&&!heap_live);fail_at=0;}
  pfa_oom=1;assert(vmo_create_contiguous(3*PAGE_SIZE,&v)==-ENOMEM&&!v&&!heap_live);pfa_oom=0;
  split_error=1;assert(vmo_create_contiguous(3*PAGE_SIZE,&v)==-EIO&&!v&&!heap_live&&!allocated_pages&&raw_frees==1);split_error=0;
  host_page_offset=0;assert(vmo_create_contiguous(3*PAGE_SIZE,&v)==-ERANGE&&!v&&!heap_live&&!allocated_pages&&raw_frees==2);host_page_offset=(uintptr_t)ram;
 }else{
  size_t request=!strcmp(name,"maximum")?(size_t)PAGE_SIZE<<MAX_ORDER:2*PAGE_SIZE+33;
  assert(!vmo_create_contiguous(request,&v)&&v);size_t rounded=(request+PAGE_SIZE-1)&~(size_t)(PAGE_SIZE-1);
  assert(v->type==VMO_CONTIGUOUS&&v->size==rounded&&v->phys_size==rounded&&allocated_pages==rounded/PAGE_SIZE);
  uint64_t pa=9;size_t extent=9;assert(!vmo_contiguous_info(v,&pa,&extent)&&!pa&&extent==rounded);
  for(size_t i=0;i<rounded;i++)assert(!ram[i]);
  for(uint32_t i=0;i<v->page_count;i++){assert(vmo_peek_page(v,i)==i&&pfa.meta[i].order==0&&pfa.meta[i].refcount==1);}
  assert(!raw_frees&&g_a20_objstats.vmos==1&&g_a20_objstats.vmo_pages==(long)(rounded/PAGE_SIZE));
  if(!strcmp(name,"immutable")){
   uint64_t flags=88;assert(vmo_resize(v,0)==-EOPNOTSUPP&&v->size==rounded&&a20_vmo_resize_lock(v,&flags)==-EOPNOTSUPP&&flags==88);
   pfn_t saved=v->pages[1];v->pages[1]=PFN_NONE;assert(a20_vmo_page_lock(v,1,&flags)==-EIO);v->pages[1]=saved;
  }else if(!strcmp(name,"pins")){
   pfn_t pages[2];assert(!vmo_pin_pages(v,1,2,pages)&&pages[0]==1&&pages[1]==2&&pfa.meta[1].refcount==2);
   ram[PAGE_SIZE]=0x5a;vmo_release(v);v=NULL;assert(allocated_pages==2&&ram[PAGE_SIZE]==0x5a&&pfa.meta[1].refcount==1);
   frame_put_many(pages,2);
  }else if(!strcmp(name,"concurrent")){
   pthread_t threads[8];for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],NULL,ref_worker,v));for(unsigned i=0;i<8;i++)assert(!pthread_join(threads[i],NULL));
  }else assert(!strcmp(name,"maximum")||!strcmp(name,"layout"));
  if(v)vmo_release(v);
  assert(!allocated_pages&&!raw_frees&&flushes==1);
 }
 assert(!allocated_pages&&!heap_live&&!g_a20_objstats.vmos&&!g_a20_objstats.vmo_pages&&!destroying);
 free(pfa.meta);free(ram);return 0;
}

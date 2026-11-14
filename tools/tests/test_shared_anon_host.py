#!/usr/bin/env python3
"""Run real VMO C adapters + Rust transactions/index with fault injection and threads."""
from pathlib import Path
import subprocess
import tempfile
from test_anon_final_destroy_host import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#define _GNU_SOURCE
#include <assert.h>
#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096u
#define PFN_NONE UINT32_MAX
#define A20_OBJ_MEMORY 1
#define A20_PERF_MM_CG_LOCK_CONTENDED 0
#define A20_PERF_MM_CG_LOCK_ACQUIRES 1
typedef uint32_t pfn_t;
typedef pthread_mutex_t spinlock_t;
typedef _Atomic unsigned refcount_t;
typedef int wait_queue_t;
struct a20_pager;
struct vmo_charge;
static void spin_init(spinlock_t *l) { assert(!pthread_mutex_init(l,NULL)); }
static uint64_t spin_lock_irqsave(spinlock_t *l) { assert(!pthread_mutex_lock(l));return 0; }
static int spin_trylock_irqsave(spinlock_t *l,uint64_t *f) { *f=0;return pthread_mutex_trylock(l)==0; }
static void spin_unlock_irqrestore(spinlock_t *l,uint64_t f) { (void)f;assert(!pthread_mutex_unlock(l)); }
static void refcount_set(refcount_t *r,unsigned n) { atomic_store(r,n); }
static void refcount_inc(refcount_t *r) { assert(atomic_fetch_add(r,1)>0); }
static int refcount_dec_and_test(refcount_t *r) { unsigned old=atomic_fetch_sub(r,1);assert(old>0);return old==1; }
static void wait_queue_init(wait_queue_t *q) { *q=0; }
/* CG_TYPE */
typedef struct cg_node {
    spinlock_t lock;
    struct { cg_mem_state_t mem; } res;
    _Atomic unsigned namespace_refs;
} cg_node_t;
static void a20_perf_count(int n) { (void)n; }
/* CG_FUNCTIONS */
/* VMO_TYPE */
static struct { _Atomic int64_t vmos,vmo_pages,vmo_dirty_frames; } g_a20_objstats;
static void a20_objstat_add(_Atomic int64_t *n,int64_t delta) { atomic_fetch_add(n,delta); }
static _Atomic int allocation_budget=-1,live_allocs,live_frames,frame_fail,flushes;
static void *kmalloc(size_t bytes) {
    int budget=atomic_load(&allocation_budget);
    if(budget==0)return NULL;
    if(budget>0)atomic_fetch_sub(&allocation_budget,1);
    void *p=malloc(bytes);assert(p);atomic_fetch_add(&live_allocs,1);return p;
}
static void *kmalloc_atomic(size_t bytes) { return kmalloc(bytes); }
static void *kcalloc_atomic(size_t n,size_t bytes) {
    void *p=kmalloc(n*bytes);if(p)memset(p,0,n*bytes);return p;
}
static void kfree(void *p) { if(p){assert(atomic_fetch_sub(&live_allocs,1)>0);free(p);} }
static void *frames[8192];
static _Atomic unsigned frame_refs[8192];
static int allocation_flush[8192];
static _Atomic unsigned next_frame;
static pfn_t pfa_alloc_page(void) {
    if(atomic_load(&frame_fail))return PFN_NONE;
    pfn_t p=atomic_fetch_add(&next_frame,1);assert(p<8192);
    frames[p]=malloc(PAGE_SIZE);assert(frames[p]);memset(frames[p],0xa5,PAGE_SIZE);
    atomic_store(&frame_refs[p],1);allocation_flush[p]=atomic_load(&flushes);atomic_fetch_add(&live_frames,1);return p;
}
static void *pfn_to_virt(pfn_t p) { assert(p<8192&&frames[p]);return frames[p]; }
static int pfn_valid(pfn_t p) { return p<8192&&frames[p]!=NULL; }
static void pfa_free_page(pfn_t p) {
    assert(pfn_valid(p));assert(atomic_load(&flushes)>allocation_flush[p]);
    free(frames[p]);frames[p]=NULL;assert(atomic_fetch_sub(&live_frames,1)>0);
}
/* Order-0 allocation and canonical pins are the model boundary. High-order
 * construction is tested separately, not executed by this shmem fixture. */
#define MAX_ORDER 11
static void frame_get(pfn_t p){assert(pfn_valid(p));assert(atomic_fetch_add(&frame_refs[p],1)>0);}
static void frame_put(pfn_t p){assert(pfn_valid(p));unsigned n=atomic_fetch_sub(&frame_refs[p],1);assert(n);if(n==1)pfa_free_page(p);}
static void frame_get_many(const pfn_t *p,size_t n){for(size_t i=0;i<n;i++)frame_get(p[i]);}
static void frame_put_many(const pfn_t *p,size_t n){for(size_t i=0;i<n;i++)frame_put(p[i]);}
static pfn_t pfa_alloc(int order){assert(order==0);return pfa_alloc_page();}
static void pfa_free(pfn_t p,int order){assert(order==0);frame_put(p);}
static int pfa_split_allocated(pfn_t p,int order){assert(order==0&&pfn_valid(p));return 0;}
static uint64_t pfn_to_phys(pfn_t p){return (uint64_t)p*PAGE_SIZE;}
void vmo_release(struct vmo *);
static void arch_tlb_flush(void) { atomic_fetch_add(&flushes,1); }
static void a20_eventq_on_object_destroy(void *v,int type) { assert(v&&type==A20_OBJ_MEMORY); }
static void a20_pager_detach_vmo(struct vmo *v) { (void)v;abort(); }
static _Noreturn void panic(const char *fmt,...) {
    va_list a;va_start(a,fmt);vfprintf(stderr,fmt,a);va_end(a);abort();
}
/* VMO_FUNCTIONS */
#define VM_VMO (UINT64_C(1)<<14)
#define VM_SYSV_SHM (UINT64_C(1)<<16)
#define VM_LOCKED (UINT64_C(1)<<15)
typedef struct { size_t locked_vm; } mm_struct_t;
typedef uintptr_t vaddr_t;
typedef struct {
    uint64_t vm_flags,backing_offset;
    uintptr_t start,end,pte_flags;
    uint32_t vmar_cap;
    int sysv_shmid;
    void *vmar_owner,*file,*file_vnode;
    struct vmo *vmo;
} mm_seg_t;
static int backing_ref_fail;
static int vma_ref_fork(mm_seg_t *v) {
    if(backing_ref_fail)return -ENOMEM;
    if((v->vm_flags&VM_VMO)&&v->vmo)vmo_ref(v->vmo);
    return 0;
}
/* REMAP_FUNCTION */
'''
TESTS = r'''
static cg_node_t groups[2];
static void check_empty(void) {
    assert(atomic_load(&live_allocs)==0&&atomic_load(&live_frames)==0);
    assert(atomic_load(&g_a20_objstats.vmos)==0&&atomic_load(&g_a20_objstats.vmo_pages)==0);
    for(int i=0;i<2;i++) {
        assert(groups[i].res.mem.rss==0&&groups[i].res.mem.shared_anon_rss==0);
        assert(atomic_load(&groups[i].namespace_refs)==0);
    }
}
static void allocation_and_accounting_failures(void) {
    atomic_store(&allocation_budget,0);
    assert(vmo_create(VMO_SHARED_ANON,PAGE_SIZE,0)==NULL);
    atomic_store(&allocation_budget,-1);check_empty();
    for(int fail=0;fail<5;fail++) {
        struct vmo *v=vmo_create(VMO_SHARED_ANON,UINT64_C(256)*1024*1024,0);assert(v&&!v->pages);
        atomic_store(&allocation_budget,fail);pfn_t p=PFN_NONE;
        assert(vmo_get_page_charged(v,65535,&groups[0],&p)==-ENOMEM);
        assert(p==PFN_NONE&&!v->phys_size&&!v->charged_pages&&!groups[0].res.mem.rss);
        atomic_store(&allocation_budget,-1);
        if(fail%2) {
            assert(vmo_get_page_charged(v,65535,&groups[0],&p)==0);
            assert(vmo_peek_page(v,65535)==p&&v->phys_size==PAGE_SIZE);
        }
        vmo_release(v);check_empty();
    }
    for(int fail=0;fail<2;fail++) {
        struct vmo *v=vmo_create(VMO_SHARED_ANON,2*PAGE_SIZE,0);pfn_t p=PFN_NONE;
        if(fail)atomic_store(&frame_fail,1);else groups[0].res.mem.limit=0;
        assert(vmo_get_page_charged(v,1,&groups[0],&p)==-ENOMEM);
        assert(!v->phys_size&&!v->charged_pages&&!groups[0].res.mem.rss);
        atomic_store(&frame_fail,0);groups[0].res.mem.limit=SIZE_MAX;
        vmo_release(v);check_empty();
    }
}
static void sparse_extent_and_charge_owners(void) {
    struct vmo *v=vmo_create(VMO_SHARED_ANON,(uint64_t)UINT32_MAX*PAGE_SIZE,0);assert(v);
    assert(atomic_load(&live_allocs)==1&&!v->pages&&!v->sparse_root);
    pfn_t a,b;assert(vmo_get_page_charged(v,7,&groups[0],&a)==0);
    assert(vmo_get_page_charged(v,0xf0000000,&groups[1],&b)==0&&a!=b);
    assert(vmo_peek_page(v,0xf0000000)==b&&vmo_peek_page(v,8)==PFN_NONE);
    assert(v->phys_size==2*PAGE_SIZE&&v->charged_pages==2&&atomic_load(&live_allocs)<16);
    assert(groups[0].res.mem.rss==1&&groups[1].res.mem.rss==1);
    assert(atomic_load(&groups[0].namespace_refs)==1&&atomic_load(&groups[1].namespace_refs)==1);
    assert(vmo_resize(v,PAGE_SIZE)==-EPERM);
    /* An exiting MM must never uncharge another still-live mapper's object. */
    cg_mem_uncharge(&groups[0],1000);assert(groups[0].res.mem.rss==1);
    groups[0].res.mem.limit=1;assert(cg_mem_charge(&groups[0],1)==-ENOMEM);
    groups[0].res.mem.limit=SIZE_MAX;
    assert(cg_mem_charge(&groups[0],3)==0&&groups[0].res.mem.rss==4);
    cg_mem_uncharge(&groups[0],3);assert(groups[0].res.mem.rss==1);
    vmo_ref(v);vmo_release(v);assert(atomic_load(&live_frames)==2);
    vmo_release(v);check_empty();
}
static void resident_backing_charge(void) {
    struct vmo *v=NULL;assert(vmo_create_resident(PAGE_SIZE+33,&groups[0],&v)==0);
    assert(v->type==VMO_RESIDENT&&v->phys_size==2*PAGE_SIZE&&v->charged_pages==2);
    assert(groups[0].res.mem.rss==2&&groups[0].res.mem.shared_anon_rss==2&&atomic_load(&groups[0].namespace_refs)==1);
    pfn_t p=PFN_NONE;assert(vmo_get_page_charged(v,0,&groups[1],&p)==0);
    assert(p!=PFN_NONE&&!groups[1].res.mem.rss&&atomic_load(&live_frames)==2);
    assert(vmo_resize(v,0)==-EOPNOTSUPP&&a20_vmo_resize_rust(v,4*PAGE_SIZE)<0&&v->size==2*PAGE_SIZE);
    cg_mem_uncharge(&groups[0],SIZE_MAX);assert(groups[0].res.mem.rss==2);
    pfn_t pages[2];assert(vmo_pin_pages(v,0,2,pages)==0);vmo_release(v);
    assert(!groups[0].res.mem.rss&&!groups[0].res.mem.shared_anon_rss&&atomic_load(&live_frames)==2);
    frame_put_many(pages,2);check_empty();
}
static void remap_backing_transfer(void) {
    for(int fail=0;fail<3;fail++) {
        struct vmo *source=vmo_create(VMO_SHARED_ANON,4*PAGE_SIZE,0);
        struct vmo *reserved=vmo_create(VMO_SHARED_ANON,PAGE_SIZE,0);
        mm_struct_t mm={0};
        mm_seg_t src={.start=0x10000,.vm_flags=VM_VMO,.vmo=source};
        mm_seg_t dst={.start=0x20000,.end=0x21000,.vm_flags=VM_VMO,.vmo=reserved};
        backing_ref_fail=fail==1;
        if(fail==2)src.backing_offset=UINT64_MAX-100;
        int rc=mm_remap_copy_backing(&mm,&dst,&src,src.start+PAGE_SIZE);
        if(!fail) {
            assert(rc==0&&dst.vmo==source&&dst.backing_offset==PAGE_SIZE);
            assert(atomic_load(&live_allocs)==1&&atomic_load(&source->refcount)==2);
            vmo_release(source);vmo_release(dst.vmo);
        } else if(fail==1) {
            assert(rc==-ENOMEM&&dst.vmo==NULL&&!(dst.vm_flags&VM_VMO));
            assert(atomic_load(&live_allocs)==1&&atomic_load(&source->refcount)==1);
            vmo_release(source);
        } else {
            assert(rc==-EINVAL&&dst.vmo==reserved&&atomic_load(&live_allocs)==2);
            vmo_release(source);vmo_release(reserved);
        }
        check_empty();
    }
    backing_ref_fail=0;
}
struct worker { struct vmo *v;int id;pfn_t p; };
static pthread_barrier_t ready,done;
static void *first_touch(void *opaque) {
    struct worker *w=opaque;
    assert(vmo_get_page_charged(w->v,0,&groups[w->id%2],&w->p)==0);
    ((unsigned char*)pfn_to_virt(w->p))[64*w->id]=(unsigned char)(w->id+1);
    pthread_barrier_wait(&ready);pthread_barrier_wait(&done);
    vmo_release(w->v);return NULL;
}
static void competing_first_touch(void) {
    enum { N=8 };pthread_t threads[N];struct worker workers[N];
    struct vmo *v=vmo_create(VMO_SHARED_ANON,PAGE_SIZE,0);assert(v);
    assert(!pthread_barrier_init(&ready,NULL,N+1)&&!pthread_barrier_init(&done,NULL,N+1));
    for(int i=0;i<N;i++) {
        vmo_ref(v);workers[i]=(struct worker){.v=v,.id=i};
        assert(!pthread_create(&threads[i],NULL,first_touch,&workers[i]));
    }
    vmo_release(v); /* the creator exits before its children finish */
    pthread_barrier_wait(&ready);
    assert(atomic_load(&live_frames)==1&&groups[0].res.mem.rss+groups[1].res.mem.rss==1);
    for(int i=0;i<N;i++) {
        assert(workers[i].p==workers[0].p);
        assert(((unsigned char*)pfn_to_virt(workers[i].p))[64*i]==i+1);
    }
    pthread_barrier_wait(&done);
    for(int i=0;i<N;i++)assert(!pthread_join(threads[i],NULL));
    pthread_barrier_destroy(&ready);pthread_barrier_destroy(&done);check_empty();
}
int main(void) {
    for(int i=0;i<2;i++){spin_init(&groups[i].lock);cg_mem_init(&groups[i].res.mem);}
    allocation_and_accounting_failures();sparse_extent_and_charge_owners();resident_backing_charge();remap_backing_transfer();
    for(int i=0;i<32;i++)competing_first_touch();
    puts("SHARED_ANON_HOST: PASS real C/Rust, sparse/resident ownership, OOM, cgroup limits/refs, 256 concurrent faulters");
    return 0;
}
'''

def main():
    source = (ROOT/'kernel/mm/vmo.c').read_text()
    source = source[source.index('extern pfn_t a20_vmo_get_page_rust'):]
    paged = extract_function(source, 'int vmo_paged_fault(')
    source = source.replace(paged, 'int vmo_paged_fault(struct vmo *v,uint32_t i,int w){(void)v;(void)i;(void)w;return -EAGAIN;}')
    header = (ROOT/'kernel/include/mm/vmo.h').read_text()
    start = header.index('struct vmo {')
    typ = header[start:header.index('\n};',start)+3]
    typ = '\n'.join(line for line in header.splitlines() if line.startswith('#define VMO_'))+'\n'+typ
    cg = (ROOT/'kernel/include/cg/cgroup.h').read_text()
    cgtyp = cg[cg.index('typedef struct cg_mem_state {'):cg.index('} cg_mem_state_t;')+len('} cg_mem_state_t;')]
    cgsource = (ROOT/'kernel/mm/cg_mem.c').read_text()
    signatures = ['void cg_mem_init(', 'static int cg_mem_charge_kind(', 'int cg_mem_charge(',
                  'int cg_mem_charge_shared_anon(', 'void cg_mem_uncharge(',
                  'void cg_mem_uncharge_shared_anon(', 'void cg_group_pin(', 'void cg_group_unpin(']
    code = PRELUDE.replace('/* CG_TYPE */',cgtyp).replace('/* VMO_TYPE */',typ)
    code = code.replace('/* CG_FUNCTIONS */','\n'.join(extract_function(cgsource,s) for s in signatures))
    code = code.replace('/* VMO_FUNCTIONS */',source)
    code = code.replace('/* REMAP_FUNCTION */',extract_function((ROOT/'kernel/mm/mremap.c').read_text(),
                                                              'static int mm_remap_copy_backing('))+TESTS
    with tempfile.TemporaryDirectory(prefix='a20-shared-anon-') as tmp:
        tmp=Path(tmp);objs=[]
        for name in ('vmo_page','vmo_lifecycle'):
            obj=tmp/(name+'.o');objs.append(str(obj))
            subprocess.run(['rustc','--edition=2021','--crate-type','lib','--emit=obj',
                            '-C','panic=abort','-C','opt-level=1',str(ROOT/f'kernel/mm/{name}.rs'),'-o',str(obj)],check=True)
        c=tmp/'test.c';exe=tmp/'test';c.write_text(code)
        subprocess.run(['cc','-std=gnu17','-O1','-g','-Wall','-Wextra','-Werror',
                        '-fsanitize=address,undefined','-pthread',str(c),*objs,'-o',str(exe)],check=True)
        subprocess.run([str(exe)],check=True,timeout=30)

if __name__ == '__main__':
    main()

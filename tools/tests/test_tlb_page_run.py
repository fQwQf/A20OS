"""Production batch hold and drain: OOM atomicity and duplicate-page pins."""
from pathlib import Path
import subprocess,tempfile
from test_pt_cache_owner import extract
from test_mmtlb_hold_release import extract_block
root=Path(__file__).resolve().parents[2]
s=(root/'kernel/mm/vm.c').read_text();header=(root/'kernel/include/mm/vm.h').read_text()
defs=extract_block(s,'enum {\n    MM_TLB_HOLD_FRAME','};')+'\n'+extract_block(header,'typedef struct mm_tlb_hold {','} mm_tlb_hold_t;')+'\n'+extract_block(s,'typedef struct mm_tlb_page_run {','} mm_tlb_page_run_t;')
code=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <errno.h>
typedef unsigned pfn_t;
#define PFN_NONE (~0u)
#define ARCH_PT_ROOT_LEVEL 3
#define ARCH_PT_ROOT_ORDER 0
typedef struct page_cache_page {void *vnode;int refs;} page_cache_page_t;
/* DEFS */
typedef struct {mm_tlb_hold_t *tlb_holds;} mm_struct_t;
static int fail,allocs,frees;
static void *kcalloc_atomic(size_t n,size_t size){if(fail)return NULL;allocs++;return calloc(n,size);}
static void kfree(void *p){frees++;free(p);}
static void page_cache_pin(page_cache_page_t *p){assert(p->refs>0);p->refs++;}
static void page_cache_put(page_cache_page_t *p){assert(p->refs>0);p->refs--;}
static void pfa_free(pfn_t p,int order){(void)p;(void)order;abort();}
static void frame_put_many(pfn_t *p,size_t n){(void)p;(void)n;abort();}
/* REAL */
int main(void){mm_struct_t mm={0};page_cache_page_t page={(void *)1,1};page_cache_page_t *pages[64];for(int i=0;i<64;i++)pages[i]=&page;
 assert(mm_tlb_hold_pages(&mm,pages,0)==-EINVAL);assert(mm_tlb_hold_pages(&mm,pages,65)==-EINVAL);pages[63]=NULL;assert(mm_tlb_hold_pages(&mm,pages,64)==-EINVAL);assert(!allocs&&page.refs==1);pages[63]=&page;
 fail=1;assert(mm_tlb_hold_pages(&mm,pages,64)==-ENOMEM);assert(!mm.tlb_holds&&page.refs==1);fail=0;
 assert(!mm_tlb_hold_pages(&mm,pages,64));assert(allocs==1&&page.refs==65&&!frees);
 assert(!mm_tlb_hold_pages(&mm,pages,3));assert(allocs==2&&page.refs==68);
 /* The live mapping releases its reference; batches must survive until drain. */
 page_cache_put(&page);assert(page.refs==67);mm_tlb_release_holds(mm.tlb_holds);assert(!page.refs&&frees==2);
 puts("TLB_PAGE_RUN: PASS validation/OOM atomicity, duplicate refs, mixed run lengths, deferred drain");}
'''.replace('/* DEFS */',defs).replace('/* REAL */',extract(s,'int mm_tlb_hold_pages(')+'\n'+extract(s,'static void mm_tlb_release_holds('))
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(code);exe=Path(d)/'test'
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

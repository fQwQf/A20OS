"""Exercise the production file-run teardown, including every hold failure."""
from pathlib import Path
import subprocess, tempfile
from test_pt_cache_owner import extract
root=Path(__file__).resolve().parents[2]
source=extract((root/'kernel/mm/munmap.c').read_text(),'int a20_munmap_file_run(')
fixture=r'''
#include <stdint.h>
#include <stddef.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#define ARCH_HAS_PGTABLE_OPS 1
#define PAGE_SIZE 4096
#define PTE_V 1
#define PTE_W 2
#define MM_ST_FILE_PRIVATE 3
#define MM_ST_GET_CLASS(x) (x)
typedef uintptr_t pte_t;
typedef uintptr_t paddr_t;
typedef struct {int pfn, refs;} page_cache_page_t;
typedef struct {int level;uint8_t cls[512];} pt_meta_t;
typedef struct {pte_t *pgdir;int rss;void *tlb_spares;} mm_struct_t;
static pte_t table[512];static pt_meta_t meta;static page_cache_page_t pages[512];
static int holds,put_count,locked,cleared,fail,noted,missing;
static pte_t *mm_pt_leaf_table(pte_t *root,uintptr_t va){(void)root;(void)va;return missing?NULL:table;}
static pt_meta_t *mm_pt_meta(pte_t *p){return p?&meta:NULL;}
static int arch_pt_vpn(uintptr_t va,int l){(void)l;return va/4096%512;}
static int arch_pt_level_entries(int l){(void)l;return 512;}
static void mm_pt_node_observe_lock(pte_t *p){assert(p==table&&!locked);locked=1;}
static void mm_pt_node_unlock(pte_t *p){assert(p==table&&locked);locked=0;}
static page_cache_page_t *mm_pt_cache_peek(pte_t *p,int i){assert(p==table&&locked);return pages[i].refs?&pages[i]:NULL;}
static int arch_pte_is_leaf(pte_t p){return !(p&4);}
static uintptr_t arch_pte_addr(pte_t p){return p&~4095UL;}
static int phys_to_pfn(uintptr_t p){return p/4096;}
static int page_cache_pfn(page_cache_page_t *p){return p->pfn;}
static int mm_tlb_hold_pages(mm_struct_t *mm,page_cache_page_t **p,size_t count){(void)mm;assert(!locked&&!cleared);if(fail)return -ENOMEM;holds=count;for(size_t i=0;i<count;i++)p[i]->refs++;return 0;}
static int pt_unmap_leaf_run_owned(mm_struct_t *mm,uintptr_t va,int count,paddr_t *pa,void **owners){(void)mm;(void)owners;assert(!locked&&holds==count);cleared=count;for(int j=0;j<count;j++){int i=va/4096+j;assert(pages[i].refs==2);pa[j]=arch_pte_addr(table[i]);table[i]=0;}return count;}
static void mm_tlb_note_change(mm_struct_t *mm,uintptr_t va,size_t size){(void)mm;(void)va;assert(size==(size_t)cleared*4096);noted++;}
static void page_cache_put(page_cache_page_t *p){assert(cleared&&p->refs==2);p->refs--;put_count++;}
static void mm_rss_sub_clamped(mm_struct_t *mm,int count){mm->rss-=count;}
/* REAL */
static void reset(void){memset(&meta,0,sizeof(meta));holds=put_count=locked=cleared=fail=noted=missing=0;for(int i=0;i<512;i++){table[i]=(i+1)*4096+PTE_V;pages[i]=(page_cache_page_t){i+1,1};meta.cls[i]=MM_ST_FILE_PRIVATE;}}
int main(void){mm_struct_t mm={.pgdir=table,.rss=512};
 for(int f=1;f<=1;f++){reset();fail=f;assert(a20_munmap_file_run(&mm,0,64*4096)==-ENOMEM);assert(!cleared&&!put_count&&!noted);for(int i=0;i<64;i++)assert(table[i]&&pages[i].refs==1);}
 reset();assert(a20_munmap_file_run(&mm,500*4096,600*4096)==12);assert(put_count==12&&noted==1&&mm.rss==500);for(int i=500;i<512;i++)assert(pages[i].refs==1); /* live TLB holds */
 for(int type=0;type<5;type++){reset();if(type==0)table[7]|=PTE_W;if(type==1)meta.cls[7]=9;if(type==2)pages[7].refs=0;if(type==3)pages[7].pfn=99;if(type==4)table[7]=0;assert(a20_munmap_file_run(&mm,0,64*4096)==7);assert(table[8]);}
 reset();missing=1;assert(!a20_munmap_file_run(&mm,0,4096*64));reset();meta.level=1;assert(!a20_munmap_file_run(&mm,0,4096*64));reset();assert(!a20_munmap_file_run(&mm,0,4096));assert(!holds);
 reset();mm.tlb_spares=&mm;assert(!a20_munmap_file_run(&mm,0,64*4096));assert(!holds&&!cleared);
 puts("MUNMAP_FILE_RUN: PASS boundaries, mixed pages, batch OOM, refs retained until shootdown");}
'''.replace('/* REAL */',source)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'probe.c';p.write_text(fixture);exe=Path(d)/'probe'
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

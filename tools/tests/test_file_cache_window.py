"""Exercise the production cache-pin batch transfer, including partial failures."""
from pathlib import Path
import subprocess, tempfile
from test_anon_final_destroy_host import extract_function
ROOT=Path(__file__).resolve().parents[2]
HARNESS=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>
#define PAGE_SIZE 4096UL
#define PFN_NONE UINT32_MAX
#define MM_ST_FILE_PRIVATE 6
#define MM_ST_VMO 3
#define PTE_W 2
#define PTE_X 8
#define PTE_COW 4
#define PTE_V 1
#define PAGE_CACHE_FAULT_AROUND_PAGES 16
#define FILE_RESIDENT_WINDOW 64
#define VM_SHARED 1
#define VM_SEALED 2
#define MM_SAFE_MASK 3
typedef uint32_t pfn_t;
typedef uintptr_t pte_t;
typedef uintptr_t vaddr_t;
typedef struct { pte_t *pgdir; } mm_struct_t;
struct page_cache_page { pfn_t pfn; int pins; };
typedef struct { struct page_cache_page **cache_owner; uint8_t safe[64], fork_file_clean, fork_file_clean_before_lock; unsigned nr_present; } pt_meta_t;
typedef struct { int node; } mm_cursor_t;
static pte_t tables[2][512];
static pt_meta_t metas[2];
static struct page_cache_page *owners[2][512];
static int held, opens, closes, writes, allocs, fail_alloc, fail_open, icache;
static int mm_addrspace_lock(mm_struct_t *m,vaddr_t s,vaddr_t e,mm_cursor_t *c) {
 (void)m;(void)s;(void)e;c->node=0;if(fail_open)return -12;opens++;return 0;
}
static pte_t *cursor_leaf_slot(mm_cursor_t *c,vaddr_t va,int create) {
 assert(!held && create);c->node=va/(512*PAGE_SIZE);assert(c->node<2);held=1;
 metas[c->node].fork_file_clean_before_lock=metas[c->node].fork_file_clean;
 metas[c->node].fork_file_clean=0;
 return &tables[c->node][va/PAGE_SIZE%512];
}
static pte_t *cursor_leaf_table(mm_cursor_t *c) { assert(held);return tables[c->node]; }
static pt_meta_t *mm_pt_meta(pte_t *p) { return &metas[p==tables[1]]; }
static int arch_pt_vpn(vaddr_t va,int l) { assert(l==0);return va/PAGE_SIZE%512; }
static int arch_pt_level_entries(int l) { assert(l==0);return 512; }
static pfn_t pfa_alloc_flags(int o,int f) { assert(held && !o && !f);return allocs++==fail_alloc?PFN_NONE:(pfn_t)(100+allocs-1); }
static void *pfn_to_virt(pfn_t p) { return p<100 ? (void *)(uintptr_t)(p+1) : owners[p-100]; }
static void arch_flush_icache_range(void *p,size_t n) { assert(p&&n==PAGE_SIZE);icache++; }
static uintptr_t pfn_to_phys(pfn_t p) { return (uintptr_t)p*PAGE_SIZE; }
static pfn_t page_cache_pfn(struct page_cache_page *p) { assert(p->pins==1);return p->pfn; }
static int pfn_valid(pfn_t p) { return p<100; }
static pte_t arch_pte_leaf(uintptr_t p,pte_t f) { return p|f; }
static int status_byte(int c,pte_t f) { assert((c==6||c==3)&&f);return c; }
static void mm_pt_note_present_meta(pt_meta_t *m,int i,int s) { assert(held&&m&&i<512&&(s==6||s==3));writes++;m->nr_present++;m->fork_file_clean=0; }
static void cursor_leaf_unlock(mm_cursor_t *c) { (void)c;assert(held);held=0; }
static void mm_cursor_unlock(mm_cursor_t *c) { (void)c;assert(!held);closes++; }
static pte_t previous;
static int private_copy;
static int arch_fork_requires_private_copy(void) { return private_copy; }
static pte_t *pt_lookup_leaf(pte_t *p,uintptr_t va,void *a,void *b,void *c) {
 (void)p;(void)va;(void)a;(void)b;(void)c;return &previous;
}
/* POLICY */
/* FUNCTION */
int main(void) {
 mm_struct_t mm={0};struct page_cache_page pages[6];struct page_cache_page *pins[6];
 for(int mode=0;mode<6;mode++) {
  memset(tables,0,sizeof(tables));memset(metas,0,sizeof(metas));
  held=opens=closes=writes=allocs=fail_open=0;fail_alloc=-1;
  for(int i=0;i<6;i++){pages[i]=(struct page_cache_page){i,1};pins[i]=&pages[i];}
  if(mode==1){tables[0][510]=0xdead;metas[0].nr_present=1;} /* Never overwrite even non-present entries. */
  if(mode==2)fail_alloc=1; /* Second leaf metadata allocation fails. */
  if(mode==4)fail_open=1;
  if(mode==5)metas[0].safe[63]=MM_SAFE_MASK;
  uintptr_t start=510*PAGE_SIZE,end=start+(mode==3?3:6)*PAGE_SIZE;
  int count=mm_map_private_cache_window(&mm,start,end,pins,6,5);
  int expected=mode==0?6:mode==1?5:mode==2?2:mode==3?3:mode==5?4:0;
  assert(count==expected&&writes==expected&&opens==closes&&!held);
  for(int i=0;i<6;i++) {
   int consumed=mode==0||(mode==1&&i!=0)||(mode==2&&i<2)||(mode==3&&i<3)||(mode==5&&i>=2);
   assert((pins[i]==NULL)==consumed);assert(pages[i].pins==1);
   if(consumed)assert(metas[(510+i)/512].cache_owner[(510+i)%512]==&pages[i]);
  }
  if(mode==1)assert(tables[0][510]==0xdead);
 }
 /* Incremental proof survives separate canonical fills, but an ordinary
  * writer invalidates it and mixed tables cannot re-establish it. */
 memset(tables,0,sizeof(tables));memset(metas,0,sizeof(metas));
 held=opens=closes=writes=allocs=fail_open=0;fail_alloc=-1;
 for(int i=0;i<6;i++){pages[i]=(struct page_cache_page){i,1};pins[i]=&pages[i];}
 assert(mm_map_private_cache_window(&mm,0,PAGE_SIZE,pins,1,5)==1);
 assert(metas[0].fork_file_clean);
 assert(mm_map_private_cache_window(&mm,PAGE_SIZE,2*PAGE_SIZE,pins+1,1,5)==1);
 assert(metas[0].fork_file_clean && metas[0].nr_present==2);
 metas[0].fork_file_clean=0; /* unrelated writer/protection/COW */
 assert(mm_map_private_cache_window(&mm,2*PAGE_SIZE,3*PAGE_SIZE,pins+2,1,5)==1);
 assert(!metas[0].fork_file_clean);
 /* Borrowed executable VMO frames use no cache-owner allocation/pin. */
 memset(tables,0,sizeof(tables));memset(metas,0,sizeof(metas));
 held=opens=closes=writes=allocs=fail_open=icache=0;fail_alloc=0;
 pfn_t borrowed[6]={0,1,2,3,4,5};
 tables[0][511]=0xbeef;
 assert(mm_map_borrowed_vmo_window(&mm,510*PAGE_SIZE,516*PAGE_SIZE,borrowed,6,13)==5);
 assert(allocs==0 && icache==5 && opens==closes && !held && writes==5);
 for(int i=0;i<6;i++) assert(borrowed[i]==(i==1?1:PFN_NONE));
 assert(tables[0][511]==0xbeef && !metas[0].cache_owner && !metas[1].cache_owner);
 /* Cache-frame aliases must never be published writable or executable. */
 for(int flags=1;flags<=15;flags++) {
  if((flags&(PTE_W|PTE_X)) || !(flags&PTE_COW))
   assert(mm_map_private_cache_window(&mm,0,6*PAGE_SIZE,pins,6,flags)==0);
 }
 previous=1;
 assert(file_fault_around_pages(&mm,0,0,1,0)==16);
 assert(file_fault_around_pages(&mm,0,PAGE_SIZE,1,0)==64);
 assert(file_fault_around_pages(&mm,0,PAGE_SIZE,1,1)==0);
 assert(file_fault_around_pages(&mm,0,PAGE_SIZE,1,VM_SEALED)==0);
 assert(file_fault_around_pages(&mm,0,PAGE_SIZE,3,0)==0);
 assert(file_fault_around_pages(&mm,0,PAGE_SIZE,9,0)==16);
 previous=0;assert(file_fault_around_pages(&mm,0,PAGE_SIZE,1,0)==16);
 previous=8;assert(file_fault_around_pages(&mm,0,PAGE_SIZE,1,0)==16);
 previous=1;private_copy=1;assert(file_fault_around_pages(&mm,0,PAGE_SIZE,1,0)==16);
 puts("FILE_CACHE_WINDOW: PASS boundaries, existing entries, partial OOM, exact pin transfer");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d)
 pt=(ROOT/'kernel/mm/pt.c').read_text()
 source='\n'.join(extract_function(pt,sig) for sig in [
  'static int map_resident_window(', 'int mm_map_private_cache_window(',
  'int mm_map_borrowed_vmo_window('])
 policy=extract_function((ROOT/'kernel/mm/fault.c').read_text(),'static int file_fault_around_pages(')
 (p/'test.c').write_text(HARNESS.replace('/* FUNCTION */',source).replace('/* POLICY */',policy))
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
 subprocess.run([str(p/'test')],check=True)

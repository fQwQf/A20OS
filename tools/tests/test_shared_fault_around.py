"""Check resident-only shared-anon batching and borrowed-frame accounting."""
from pathlib import Path
import subprocess, tempfile
from test_anon_final_destroy_host import extract_function
ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define PAGE_SIZE 4096UL
#define PFN_NONE UINT32_MAX
#define MM_ST_VMO 3
typedef uint32_t pfn_t;
typedef struct { uint64_t size; } vmo_t;
typedef struct { uint64_t end, pte_flags; vmo_t *vmo; } mm_seg_t;
typedef struct { int rss; } mm_struct_t;
typedef struct { int unused; } mm_cursor_t;
static pfn_t canonical[64];
static uint64_t mapped[64], low, high;
static int held, opens, fail_open, fail_slot;
static pfn_t vmo_peek_page(vmo_t *vmo, uint32_t index) {
 assert(!held && index*PAGE_SIZE < vmo->size);return canonical[index];
}
static uintptr_t pfn_to_phys(pfn_t pfn) { return (uintptr_t)pfn*PAGE_SIZE; }
static int mm_addrspace_lock(mm_struct_t *mm,uint64_t start,uint64_t end,mm_cursor_t *cur) {
 (void)mm;(void)cur;assert(!held);if(fail_open)return -ENOMEM;
 held=1;opens++;low=start;high=end;return 0;
}
static int mm_cursor_map_if_absent(mm_cursor_t *cur,uint64_t va,uintptr_t pa,uint64_t flags,int cls) {
 (void)cur;assert(held && va>=low && va<high && flags==7 && cls==MM_ST_VMO);
 unsigned slot=va/PAGE_SIZE;if((int)slot==fail_slot)return -ENOMEM;
 if(mapped[slot])return -EEXIST; mapped[slot]=pa;return 0;
}
static void mm_cursor_unlock(mm_cursor_t *cur) { (void)cur;assert(held);held=0; }
static void mm_rss_add(mm_struct_t *mm,int n) { assert(!held);mm->rss+=n; }
static int mm_map_borrowed_vmo_window(mm_struct_t *mm,uint64_t start,uint64_t end,
                                      pfn_t *pfns,size_t count,uint64_t flags) {
 mm_cursor_t cur;
 if(mm_addrspace_lock(mm,start,end,&cur))return 0;
 int n=0;
 for(size_t i=0;i<count;i++) {
  int r=mm_cursor_map_if_absent(&cur,start+i*PAGE_SIZE,pfn_to_phys(pfns[i]),flags,MM_ST_VMO);
  if(r==-EEXIST)continue;if(r<0)break;n++;
 }
 mm_cursor_unlock(&cur);return n;
}
/* FUNCTION */
int main(void) {
 for(int mode=0;mode<7;mode++) {
  vmo_t vmo={64*PAGE_SIZE};mm_seg_t vma={64*PAGE_SIZE,7,&vmo};mm_struct_t mm={0};
  memset(mapped,0,sizeof(mapped));for(int i=0;i<64;i++)canonical[i]=100+i;
  held=opens=fail_open=0;fail_slot=-1;uint64_t offset=0;
  if(mode==1)canonical[1]=PFN_NONE;
  if(mode==2)mapped[5]=999*PAGE_SIZE;
  if(mode==3)fail_slot=5;
  if(mode==4)vma.end=4*PAGE_SIZE;
  if(mode==5){vmo.size=5*PAGE_SIZE;offset=3*PAGE_SIZE;}
  if(mode==6)fail_open=1;
  shared_anon_fault_around(&mm,&vma,0,offset);
  int expected=mode==0?31:mode==1?0:mode==2?30:mode==3?4:mode==4?3:mode==5?1:0;
  assert(mm.rss==expected && !held && opens==(expected!=0));
  if(expected)assert(mapped[1]==(101+offset/PAGE_SIZE)*PAGE_SIZE);
  if(mode==2)assert(mapped[5]==999*PAGE_SIZE);
  if(mode==3)assert(!mapped[5]);
  for(int i=0;i<64;i++)assert(canonical[i]==((mode==1&&i==1)?PFN_NONE:(pfn_t)(100+i)));
 }
 puts("SHARED_FAULT_AROUND: PASS holes, bounds, offsets, races, partial failure, lock order");
}
'''
with tempfile.TemporaryDirectory() as d:
    p=Path(d)
    source=extract_function((ROOT/'kernel/mm/fault.c').read_text(),'static void shared_anon_fault_around(')
    (p/'test.c').write_text(HARNESS.replace('/* FUNCTION */',source))
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-Wno-misleading-indentation','-fsanitize=address,undefined',str(p/'test.c'),'-o',str(p/'test')],check=True)
    subprocess.run([str(p/'test')],check=True)

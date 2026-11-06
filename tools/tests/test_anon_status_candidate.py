"""Real negative-only preflight preserves proof and owns read-side/node guards."""
from pathlib import Path
import subprocess,tempfile
from test_pt_cache_owner import extract
ROOT=Path(__file__).resolve().parents[2]
s=extract((ROOT/'kernel/mm/pt.c').read_text(),'int mm_pt_anon_status_candidate(')
h=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define PTE_V 1
#define MM_ST_ANON_VIRT 1
#define MM_ST_GET_CLASS(b) ((b)>>4)
typedef uintptr_t pte_t;typedef uintptr_t vaddr_t;
typedef struct {pte_t *pgdir;} mm_struct_t;
typedef struct {int level,stale,proof;unsigned char cls[512];} pt_meta_t;
static pte_t table[512];static pt_meta_t meta;static int read_side,locked,missing,remove_on_lock;
static void mm_pt_read_enter(mm_struct_t *m){assert(m&&!read_side);read_side=1;}
static void mm_pt_read_exit(mm_struct_t *m){assert(m&&read_side&&!locked);read_side=0;}
static pte_t *mm_pt_leaf_table(pte_t *p,vaddr_t va){assert(read_side&&p&&va<512*4096);return missing?NULL:table;}
static pt_meta_t *mm_pt_meta(pte_t *p){assert(read_side);return p?&meta:NULL;}
static void mm_pt_node_observe_lock(pte_t *p){assert(p&&read_side&&!locked);locked=1;if(remove_on_lock)meta.stale=1;}
static void mm_pt_node_unlock(pte_t *p){assert(p&&locked);locked=0;}
static int arch_pt_vpn(vaddr_t va,int level){assert(!level);return va/4096;}
/* REAL */
int main(void){mm_struct_t mm={table};meta.proof=1;
 assert(!mm_pt_anon_status_candidate(NULL,0));
 for(int cls=0;cls<10;cls++)for(int present=0;present<2;present++){
  meta.cls[3]=cls<<4;table[3]=present;assert(mm_pt_anon_status_candidate(&mm,3*4096)==(cls==1&&!present));assert(meta.proof&&!read_side&&!locked);
 }
 meta.cls[3]=1<<4;table[3]=0;missing=1;assert(!mm_pt_anon_status_candidate(&mm,3*4096));missing=0;
 meta.level=1;assert(!mm_pt_anon_status_candidate(&mm,3*4096));meta.level=0;
 remove_on_lock=1;assert(!mm_pt_anon_status_candidate(&mm,3*4096));assert(!read_side&&!locked&&meta.proof);
 puts("ANON_STATUS_CANDIDATE: PASS all classes, present/missing/huge/stale and proof preservation");return 0;
}
'''.replace('/* REAL */',s)
with tempfile.TemporaryDirectory() as d:
 p=Path(d)/'test.c';p.write_text(h);exe=Path(d)/'test'
 subprocess.run(['cc','-O2','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p),'-o',str(exe)],check=True)
 subprocess.run([str(exe)],check=True)

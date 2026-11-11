"""Compile the real annotation transaction: full entries must not retain VMAs."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
MODEL=r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <assert.h>
#include <errno.h>
#define MM_SEGTAB_MAX 255
#define MM_SEGTAB_NAMES 2
#define MM_PT_META_ENTRIES 4
#define ARCH_PT_ROOT_LEVEL 1
#define PTE_V 1
typedef uintptr_t pte_t;typedef uintptr_t vaddr_t;
typedef struct {int refs;} mm_seg_t;
struct mm_segarr {int refcount;uint8_t n;mm_seg_t *seg[MM_SEGTAB_MAX];};
typedef struct {struct mm_segarr *arr;uint8_t *idx;} mm_segtab_t;
typedef struct {int stale;mm_segtab_t *segtab;} pt_meta_t;
static pt_meta_t meta;static uint64_t mm_seg_annot_lost[2],mm_seg_full_lvl[8];
static pt_meta_t *mm_pt_meta(pte_t *t){(void)t;return &meta;}
static void mcs_lock(pt_meta_t *m){(void)m;}
static void mcs_unlock(pt_meta_t *m){(void)m;}
static int arch_pt_vpn(vaddr_t a,int l){(void)l;return (a>>12)&3;}
static int arch_pt_level_entries(int l){(void)l;return 4;}
static int arch_pte_is_leaf(pte_t e){(void)e;return 0;}
static pte_t *arch_pte_to_ptr(pte_t e){return (pte_t*)e;}
static mm_segtab_t *segtab_attach_locked(pt_meta_t *m){return m->segtab;}
static mm_seg_t *mm_seg_get(mm_seg_t *s){s->refs++;return s;}
'''
MAIN=r'''
int main(void){
 struct mm_segarr arr={0};uint8_t idx[8]={0};mm_segtab_t st={&arr,idx};meta.segtab=&st;
 pte_t table[4]={1,1,1,1};mm_seg_t live[3]={{1},{1},{1}};
 assert(seg_annotate_rec(table,1,0,4096,&live[0],0)==1);
 assert(seg_annotate_rec(table,1,0,4096,&live[1],0)==1);
 assert(arr.n==2&&live[0].refs==2&&live[1].refs==2);
 for(int j=0;j<1000;j++){
  mm_seg_t transient={1};seg_annotate_rec(table,1,0,4096,&transient,0);
  assert(transient.refs==1&&arr.n==2);assert(idx[0]==1&&idx[1]==2);
 }
 assert(mm_seg_annot_lost[1]==1000&&mm_seg_annot_lost[0]==0);
 seg_annotate_rec(table,1,0,4096,&live[0],0);assert(live[0].refs==2&&arr.n==2);
 assert(seg_annotate_rec(table,1,4096,8192,&live[2],0)==1);
 assert(live[2].refs==2&&arr.n==3&&idx[2]==3);
 seg_annotate_rec(table,1,0,4096,&live[2],0);assert(live[2].refs==2&&arr.n==3);
 return 0;
}
'''
class SegtabPublication(unittest.TestCase):
    def test_full_entry_rolls_back_before_reference_acquisition(self):
        code=(ROOT/'kernel/mm/pt.c').read_text()
        names=['segtab_slot_get','segtab_slot_set','segtab_slot','segtab_entry_name',
               'segtab_entry_names','segtab_entry_can_name','seg_annotate_rec']
        funcs='\n'.join(function(code,name) for name in names)
        with tempfile.TemporaryDirectory() as temp:
            p=Path(temp)
            def run(body,name):
                source,binary=p/(name+'.c'),p/name;source.write_text(MODEL+body+MAIN)
                subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-Wno-unused-function',
                                '-fsanitize=undefined',str(source),'-o',str(binary)],check=True,capture_output=True)
                return subprocess.run([str(binary)],capture_output=True).returncode
            self.assertEqual(run(funcs,'fixed'),0)
            old=funcs.replace('st && !segtab_entry_can_name(st, i, seg)','0')
            self.assertNotEqual(old,funcs);self.assertNotEqual(run(old,'old'),0)
if __name__=='__main__':unittest.main()

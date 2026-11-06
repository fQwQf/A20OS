"""A real private-file COW status update must retire the cache ownership."""
from pathlib import Path
import subprocess, tempfile
from test_cow_anon_child_batch import extract_block
ROOT=Path(__file__).resolve().parents[2]
source=extract_block((ROOT/'kernel/mm/fault.c').read_text(),'static void cow_sync_status(')
harness=r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#define ARCH_HAS_PGTABLE_OPS 1
#define VM_FILE 1
#define VM_SHARED 2
#define VM_VMO 4
#define MM_ST_INVALID 0
#define MM_ST_PT_NODE 1
#define MM_ST_ANON_MAPPED 2
#define MM_ST_VMO 3
#define MM_ST_FILE_PRIVATE 4
#define MM_ST_FILE_SHARED 5
#define MM_ST_CLS_BYTE(x) ((x)<<4)
#define MM_ST_GET_CLASS(x) ((x)>>4)
#define MM_ST_COW_BIT 8
#define PTE_COW 8
typedef uintptr_t pte_t;
typedef uintptr_t vaddr_t;
typedef struct mm_struct { pte_t *pgdir; } mm_struct_t;
typedef struct { unsigned vm_flags; } mm_seg_t;
static pte_t table[1];
static uint8_t status;
static void *owner;
static pte_t *pt_lookup_leaf(pte_t *root,vaddr_t va,int *level,void *a,void *b) {
 (void)va;(void)a;(void)b;assert(root==table);*level=0;return table;
}
static pte_t *mm_pt_leaf_table(pte_t *root,vaddr_t va) { (void)va;return root; }
static int arch_pt_vpn(vaddr_t va,int l) { (void)va;assert(!l);return 0; }
static uint8_t mm_pt_peek(pte_t *t,int l,int i) { assert(t==table&&!l&&!i);return status; }
static uint8_t mm_pt_prot_bits(pte_t pte) { return pte&7; }
static int mm_pt_sync_status(pte_t *t,int l,int i,uint8_t cls) {
 assert(t==table&&!l&&!i);status=MM_ST_CLS_BYTE(cls)|mm_pt_prot_bits(*t);return 0;
}
static void mm_pt_note_present(pte_t *t,int l,int i,uint8_t byte) {
 assert(t==table&&!l&&!i);owner=NULL;status=byte;
}
/* BODY */
int main(void) {
 mm_struct_t mm={table};mm_seg_t vma={VM_FILE};table[0]=3;
 for(int copied=0;copied<2;copied++) {
  owner=&vma;status=MM_ST_CLS_BYTE(MM_ST_FILE_PRIVATE);
  cow_sync_status(&mm,0,&vma,copied);
  assert(MM_ST_GET_CLASS(status)==(copied?MM_ST_ANON_MAPPED:MM_ST_FILE_PRIVATE));
  assert((owner==NULL)==copied);
 }
 vma.vm_flags=VM_FILE|VM_SHARED;owner=&vma;status=MM_ST_CLS_BYTE(MM_ST_FILE_SHARED);
 cow_sync_status(&mm,0,&vma,1);
 assert(owner==&vma&&MM_ST_GET_CLASS(status)==MM_ST_FILE_SHARED);
 puts("COW_PRIVATE_COPY_STATUS: PASS ownership transfer, private/shared classification");
}
'''
with tempfile.TemporaryDirectory() as d:
 p=Path(d);(p/'probe.c').write_text(harness.replace('/* BODY */',source))
 subprocess.run(['cc','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(p/'probe.c'),'-o',str(p/'probe')],check=True)
 subprocess.run([str(p/'probe')],check=True)

#!/usr/bin/env python3
"""Inject PT allocation failures into the production MMIO transaction/unmap.

Small page-table geometry makes both intermediate allocation boundaries cheap
and deterministic. PTE clearing/empty-table retirement and the mapping transaction
are extracted from production; only allocation, locking and hardware are stubs.
"""
from pathlib import Path
import subprocess
import tempfile
from test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#define PAGE_SIZE 4096
#define ARCH_PT_ROOT_LEVEL 2
#define PTE_V 1
#define PTE_LEAF 2
#define PTE_R 4
#define PTE_W 8
#define PTE_U 16
#define PTE_A 32
#define PTE_D 64
#define VM_SHARED 1
#define VM_PFNMAP 2
#define VM_DONTFORK 4
#define VM_READ 8
#define VM_WRITE 16
#define KLOG_WARN 1
#define klog(...) ((void)0)
typedef uint64_t pte_t;
typedef uint64_t vaddr_t;
typedef struct { uint64_t start, end, vm_flags; } mm_seg_t;
typedef struct { pte_t *pgdir; int lock; uint64_t mmap_base; mm_seg_t *vma; } mm_struct_t;
static int live_tables, live_vmas, transaction, changes, retired, fail_after, allocations;
static pte_t *retire_list[16];
static int arch_pt_vpn(uint64_t va, int level) { return (va >> (12+2*level)) & 3; }
static int arch_pt_level_entries(int level) { (void)level; return 4; }
static int arch_pte_is_leaf(pte_t p) { return p & PTE_LEAF; }
static pte_t *arch_pte_to_ptr(pte_t p) { return (pte_t *)(uintptr_t)(p & ~4095ULL); }
static void mm_pt_node_lock(pte_t *p) { (void)p; assert(transaction); }
static void mm_pt_node_unlock(pte_t *p) { (void)p; }
static void mm_pt_note_absent(pte_t *p, int l, int i) { (void)p; (void)l; (void)i; }
static void mm_pt_node_clear_seg(pte_t *p, int l, int i) { (void)p; (void)l; (void)i; }
static void mm_pt_retire_table(mm_struct_t *mm, pte_t *p, int level) {
    (void)level; assert(mm->lock && transaction); retire_list[retired++] = p;
}
static pte_t *new_table(void) {
    if (fail_after >= 0 && allocations++ == fail_after) return NULL;
    pte_t *p = aligned_alloc(4096, 4096); assert(p); memset(p, 0, 4096);
    live_tables++; return p;
}
static int pt_map(pte_t *root, uint64_t va, uint64_t pa, pte_t flags) {
    pte_t *table = root;
    for (int l=2; l>0; l--) {
        int i = arch_pt_vpn(va, l);
        if (!(table[i] & PTE_V)) {
            pte_t *p = new_table(); if (!p) return -ENOMEM;
            table[i] = (uintptr_t)p | PTE_V;
        }
        table = arch_pte_to_ptr(table[i]);
    }
    assert(!table[arch_pt_vpn(va, 0)]);
    table[arch_pt_vpn(va, 0)] = pa | flags; return 0;
}
static pte_t leaf(pte_t *table, uint64_t va) {
    for (int l=2; l>0; l--) {
        pte_t p = table[arch_pt_vpn(va, l)];
        if (!(p & PTE_V)) return 0;
        table = arch_pte_to_ptr(p);
    }
    return table[arch_pt_vpn(va, 0)];
}
static int udriver_mmio_allowed(uint64_t pa, uint64_t size) {
    return pa >= 0x100000 && pa + size <= 0x200000;
}
static mm_seg_t *mm_seg_new(void) { live_vmas++; return calloc(1, sizeof(mm_seg_t)); }
static void spin_lock(int *lock) { assert(transaction && !*lock); *lock=1; }
static void spin_unlock(int *lock) { assert(*lock); *lock=0; }
static void mm_tlb_invalidate_begin(mm_struct_t *mm) { assert(!mm->lock && !transaction); transaction=1; }
static void mm_tlb_note_change(mm_struct_t *mm, uint64_t va, uint64_t size) {
    (void)va; (void)size; assert(mm->lock && transaction); changes++;
}
static void mm_tlb_invalidate_finish(mm_struct_t *mm) {
    assert(!mm->lock && transaction);
    /* No concurrent walkers in this harness; consume queued retirement
     * here and independently assert the driver finishes its TLB transaction. */
    for (int i=0; i<retired; i++) { free(retire_list[i]); live_tables--; }
    retired=0; transaction=0;
}
static uint64_t mm_find_gap(mm_struct_t *mm, uint64_t base, uint64_t size) {
    (void)size; assert(mm->lock); return base;
}
static void mm_insert_vma(mm_struct_t *mm, mm_seg_t *vma) {
    assert(mm->lock && !mm->vma); mm->vma=vma;
}
static void vma_put(mm_struct_t *mm, mm_seg_t *vma) {
    assert(!mm->lock && !transaction); free(vma); live_vmas--;
}
static void mm_vma_flush_deferred(mm_struct_t *mm) { assert(!mm->lock && !transaction); }
'''
HARNESS = r'''
int main(void) {
    /* Boundary at L0 (4 pages) and L1 (16 pages); the target starts
     * one page before it, so failure can follow a populated prefix. */
    for (int boundary=4; boundary<=16; boundary*=4) {
        for (int fail=0; fail<4; fail++) {
            fail_after=-1; allocations=0; changes=0;
            pte_t *root = new_table();
            mm_struct_t mm = { .pgdir=root, .mmap_base=(boundary-1)*PAGE_SIZE };
            assert(pt_map(root, 0, 0x300000, PTE_V|PTE_LEAF|PTE_R)==0);
            int baseline=live_tables;
            /* A nearby survivor shares some parent tables with rollback. */
            pte_t neighbor=leaf(root, 0);
            fail_after=fail; allocations=0;
            uint64_t out=0xfeed;
            int r=udriver_map_mmio(&mm, 0x100000, 3*PAGE_SIZE, 3, &out);
            if (r<0) {
                if (out!=0xfeed || live_vmas || mm.vma || live_tables!=baseline) return 1;
                for (int i=0;i<3;i++) if (leaf(root, mm.mmap_base+i*PAGE_SIZE)) return 2;
            } else {
                assert(mm.vma && out==mm.mmap_base && changes==1);
                for (int i=0;i<3;i++) assert(leaf(root, out+i*PAGE_SIZE));
                mm_tlb_invalidate_begin(&mm); spin_lock(&mm.lock);
                for (int i=0;i<3;i++) assert(pt_unmap(&mm, out+i*PAGE_SIZE)==0);
                spin_unlock(&mm.lock); mm_tlb_invalidate_finish(&mm);
                vma_put(&mm, mm.vma); mm.vma=NULL;
                assert(live_tables==baseline && live_vmas==0);
            }
            assert(leaf(root, 0)==neighbor && !transaction && !mm.lock);
            mm_tlb_invalidate_begin(&mm); spin_lock(&mm.lock);
            assert(pt_unmap(&mm, 0)==0);
            spin_unlock(&mm.lock); mm_tlb_invalidate_finish(&mm);
            free(root); live_tables--;
            assert(!live_tables);
        }
    }
    return 0;
}
'''


def main():
    mm = (ROOT / 'kernel/mm/mm.c').read_text()
    unmap = extract_function(mm, 'int pt_unmap(mm_struct_t *mm, vaddr_t va)')
    helpers = extract_function(mm, 'static int pt_entry_mapped(') + '\n' + extract_function(mm, 'static int pt_table_empty(pte_t *table, int level, int cleared_idx) {')
    driver = extract_function((ROOT / 'kernel/drivers/core/udriver.c').read_text(), 'int udriver_map_mmio(')
    no_rollback = driver.replace('(void)pt_unmap(mm, va + mapped);', '(void)0;')
    no_prune = unmap.replace('if (!(pte & PTE_V)) break;', 'if (!(pte & PTE_V)) return -EINVAL;')
    assert no_rollback != driver and no_prune != unmap
    with tempfile.TemporaryDirectory(prefix='a20-mmio-rollback-') as tmp:
        for name, d, u, expected in [('current', driver, unmap, 0),
                                     ('no-rollback', no_rollback, unmap, 2),
                                     ('no-empty-prune', driver, no_prune, 1)]:
            src, exe = Path(tmp) / (name + '.c'), Path(tmp) / name
            src.write_text(PRELUDE + helpers + '\n' + u + '\n' + d + HARNESS)
            subprocess.run(['cc', '-std=c11', '-Wall', '-Wextra', '-Werror', str(src), '-o', str(exe)], check=True)
            result = subprocess.run([str(exe)], check=False)
            if result.returncode != expected:
                raise SystemExit(f'{name}: expected {expected}, got {result.returncode}')
    print('udriver-mmio-rollback: PASS (production mapper/unmap, PT allocation faults, negative controls)')


if __name__ == '__main__':
    main()

"""Inject migration and an unfinished fork shootdown into real MM paths."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


def run(harness):
    with tempfile.TemporaryDirectory() as tmp:
        source, binary = Path(tmp) / 'probe.c', Path(tmp) / 'probe'
        source.write_text(harness)
        subprocess.run(['cc', '-Wall', '-Wextra', '-Werror', str(source),
                        '-o', str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


class TlbTransactionRaces(unittest.TestCase):
    def test_local_flush_generation_cannot_migrate(self):
        text = (ROOT / 'kernel/mm/vm.c').read_text()
        start = text.index('void mm_tlb_invalidate_finish(')
        end = text.index('    uint64_t flags = spin_lock_irqsave', start)
        body = text[start:end] + '}\n'
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define CONFIG_NR_CPUS 3
#define PAGE_SIZE 4096
#define a20_perf_count(...) ((void)0)
#define a20_perf_add(...) ((void)0)
typedef struct {
    uint64_t tlb_generation, tlb_cpu_generation[3], tlb_start, tlb_end;
    unsigned active_cpus, tlb_pending;
} mm_struct_t;
static unsigned pinned, cpu, flushed;
static void preempt_disable(void) { assert(!pinned); pinned=1; }
static void preempt_enable(void) { assert(pinned); pinned=0; }
static int smp_remote_tlb_flush_supported(void) { return 1; }
static unsigned smp_online_cpu_mask(void) { return 7; }
static void arch_mb(void) {}
static unsigned cpu_current_id(void) {
    unsigned before=cpu;
    if (!pinned) cpu=1; /* migrate immediately after identity was sampled */
    return before;
}
static void arch_tlb_flush_page_local(uint64_t addr) { (void)addr; flushed |= 1U<<cpu; }
static void arch_tlb_flush_local(void) { flushed |= 1U<<cpu; }
static void arch_tlb_flush(void) { flushed=7; }
static int smp_remote_tlb_flush(unsigned targets, uint64_t start, uint64_t size) {
    (void)start; (void)size; flushed |= targets; return 0;
}
static void panic(const char *s) { (void)s; assert(0); }
''' + body + r'''
int main(void) {
    mm_struct_t mm={.tlb_pending=1,.tlb_end=4096,.active_cpus=7};
    mm_tlb_invalidate_finish(&mm);
    assert(flushed==7 && !pinned);
    for (unsigned i=0;i<3;i++) assert(mm.tlb_cpu_generation[i]==1);
    return 0;
}
''')

    def test_cow_waits_for_fork_write_protection(self):
        text = (ROOT / 'kernel/mm/fault.c').read_text()
        start = text.index('int handle_cow_fault(task_t *t,')
        end = text.index('\n}', start) + 2
        body = text[start:end]
        run(r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#define ARCH_HAS_PGTABLE_OPS 1
#define PAGE_SIZE 4096
#define PFN_NONE (~0UL)
#define a20_perf_count(...) ((void)0)
typedef unsigned long pfn_t;
typedef int page_cache_page_t;
typedef struct { int lock, tlb_lock; } mm_struct_t;
typedef struct { mm_struct_t *mm; uint64_t perf_page_faults; } task_t;
static uint64_t g_perf_sw_page_faults;
static int pending=1, old_value=4, copied, flushed, mode;
static void mutex_lock(int *lock) {
    assert(!*lock); *lock=1;
    /* A sibling writes through the old writable TLB before fork's flush. */
    if (pending) { old_value=5; pending=0; }
}
static void mutex_unlock(int *lock) { assert(*lock); *lock=0; }
static void spin_lock(int *lock) { assert(!*lock); *lock=1; }
static void spin_unlock(int *lock) { assert(*lock); *lock=0; }
static int mm_cow_from_status(task_t *t, mm_struct_t *m, uint64_t va, pfn_t *old) {
    (void)t; (void)va; assert(m->tlb_lock && m->lock && !pending);
    if (mode) return 0;
    copied=old_value; *old=1; return 1;
}
static int handle_cow_fault_locked(task_t *t,uint64_t va,pfn_t *old,page_cache_page_t **page) {
    (void)va; (void)old; (void)page; assert(t->mm->tlb_lock && !pending);
    copied=old_value; return 0;
}
static void mm_tlb_shootdown_page(mm_struct_t *m,uint64_t va) {
    (void)va; assert(m->tlb_lock && !m->lock); flushed=1;
}
static void frame_put(pfn_t frame) { (void)frame; assert(flushed); }
static void page_cache_put(page_cache_page_t *p) { (void)p; assert(flushed); }
''' + body + r'''
int main(void) {
    mm_struct_t mm={0}; task_t t={.mm=&mm};
    for (mode=0;mode<2;mode++) {
        pending=1; old_value=4; copied=flushed=0;
        assert(handle_cow_fault(&t,4096)==0);
        assert(copied==5 && flushed && !mm.lock && !mm.tlb_lock);
    }
    return 0;
}
''')

    def test_fork_between_initial_cow_and_present_recheck(self):
        text = (ROOT / 'kernel/mm/fault.c').read_text()
        start = text.index('int handle_present_page_fault(')
        end = text.index('\n}', start) + 2
        run(r''' 
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
typedef unsigned long pte_t;
#define PTE_V 1
#define PTE_U 2
#define PTE_W 4
#define PTE_R 8
#define PTE_X 16
#define PTE_A 32
#define PTE_COW 64
enum mm_fault_access { MM_FAULT_ACCESS_READ, MM_FAULT_ACCESS_WRITE, MM_FAULT_ACCESS_EXEC };
typedef struct { unsigned vm_flags; } mm_seg_t;
typedef struct { int lock; pte_t *pgdir; } mm_struct_t;
typedef struct { mm_struct_t *mm; } task_t;
static mm_seg_t vma={PTE_W|PTE_R};
static unsigned copies;
static void spin_lock(int *p) { assert(!*p); *p=1; }
static void spin_unlock(int *p) { assert(*p); *p=0; }
static pte_t *pt_lookup_leaf(pte_t *p,uint64_t va,void *a,void *b,void *c) {
    (void)va; (void)a; (void)b; (void)c; return p;
}
static int arch_pte_is_leaf(pte_t p) { return (p&PTE_V)!=0; }
static mm_seg_t *mm_seg_find(mm_struct_t *m,uint64_t va) { (void)m; (void)va; return &vma; }
static int fault_access_allowed(unsigned flags,enum mm_fault_access access) {
    return (flags & (access==MM_FAULT_ACCESS_WRITE?PTE_W:PTE_R))!=0;
}
static void arch_tlb_flush_page_local(uint64_t va) { (void)va; }
static int handle_cow_fault(task_t *t,uint64_t va) {
    (void)va; assert(!t->mm->lock); copies++; return 0;
}
''' + text[start:end] + r'''
int main(void) {
    pte_t pte=PTE_V|PTE_U|PTE_R|PTE_COW;
    mm_struct_t mm={.pgdir=&pte}; task_t t={&mm};
    assert(handle_present_page_fault(&t,4096,MM_FAULT_ACCESS_WRITE)==0 && copies==1);
    vma.vm_flags=PTE_R;
    assert(handle_present_page_fault(&t,4096,MM_FAULT_ACCESS_WRITE)==-1 && copies==1);
    return 0;
}
''')


if __name__ == '__main__':
    unittest.main()

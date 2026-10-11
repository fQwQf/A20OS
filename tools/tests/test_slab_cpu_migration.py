"""Inject migration at every IRQ-enable boundary in the real slab slow paths."""
from pathlib import Path
import os
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class SlabCpuMigration(unittest.TestCase):
    def test_refill_and_drain_keep_the_selected_cpu(self):
        source = Path(os.environ.get('SLAB_TEST_SOURCE', ROOT / 'kernel/mm/slab.c')).read_text()
        allocation = source[source.index('    int idx = 0;', source.index('void *kmalloc_flags(')):
                            source.index('    if (!obj && can_reclaim)', source.index('void *kmalloc_flags('))]
        release_start = source.index('    int idx = sp->cache_idx;', source.index('void kfree('))
        release = source[release_start:source.index('\n}', release_start)]
        harness = r'''
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#define SLAB_NR_CACHES 1
#define SLAB_CPU_REFILL 8
#define SLAB_CPU_ARRAY_CAP 16
typedef struct { void *objs[16]; unsigned count; } slab_cpu_array_t;
typedef struct { unsigned lock; } slab_cache_t;
typedef struct { unsigned cache_idx; } slab_page_t;
static slab_cache_t caches[1];
static size_t slab_sizes[1]={512};
static slab_cpu_array_t arrays[2];
static unsigned cpu, selected, irq=1, serial, frees;
static int arch_irqs_enabled(void) { return irq; }
static void arch_local_irq_disable(void) { irq=0; }
static void arch_local_irq_enable(void) { irq=1;cpu^=1; }
static slab_cpu_array_t *slab_cpu_array(int idx) {
    assert(!irq && idx==0);selected=cpu;return &arrays[cpu];
}
static void spin_lock(unsigned *lock) {
    assert(!irq && selected==cpu && !*lock);*lock=1;
}
static uint64_t spin_lock_irqsave(unsigned *lock) {
    unsigned before=irq;arch_local_irq_disable();spin_lock(lock);return before;
}
static void spin_unlock_irqrestore(unsigned *lock,uint64_t flags) {
    assert(!irq && selected==cpu && *lock);*lock=0;
    if(flags)arch_local_irq_enable();
}
static void *slab_alloc_obj_locked(int idx) {
    assert(idx==0 && !irq && caches[0].lock && selected==cpu);
    return (void*)(uintptr_t)(++serial*16);
}
static void slab_free_obj_locked(void *p,uintptr_t caller) {
    (void)caller;assert(p && !irq && selected==cpu && caches[0].lock);frees++;
}
static void *allocate(size_t size) {
''' + allocation + r'''
    return obj;
}
static void release(void *ptr) {
    slab_page_t page={0},*sp=&page;uintptr_t caller_ra=0;
''' + release + r'''
}
int main(void) {
    arrays[1].count=3;
    void *p;
#if !PROBE_FREE
    p=allocate(512);
    assert(p && irq && cpu==1 && arrays[0].count==8 && arrays[1].count==3);
#endif
    cpu=0;arrays[0].count=16;
    for(unsigned i=0;i<16;i++)arrays[0].objs[i]=(void*)(uintptr_t)((i+1)*16);
    release((void*)0x1000);
    assert(irq && cpu==1 && arrays[0].count==9 && arrays[1].count==3 && frees==8);
    /* Entering with IRQs already disabled must not enable them on return. */
    cpu=0;irq=0;arrays[0].count=0;p=allocate(512);assert(p && !irq && cpu==0);
    arrays[0].count=16;release((void*)0x1000);assert(!irq && cpu==0);
    return 0;
}
'''
        with tempfile.TemporaryDirectory() as tmp:
            src, binary = Path(tmp) / 'slab.c', Path(tmp) / 'slab'
            src.write_text(harness)
            for probe in (0, 1):
                with self.subTest(path='drain' if probe else 'refill'):
                    subprocess.run(['cc', '-Wall', '-Wextra', '-Werror',
                                    '-Wno-unused-function', f'-DPROBE_FREE={probe}',
                                    str(src), '-o', str(binary)], check=True)
                    subprocess.run([str(binary)], check=True)


if __name__ == '__main__':
    unittest.main()

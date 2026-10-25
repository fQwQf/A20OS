"""Execute the common MCU allocator with a board-supplied external arena."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include "core/types.h"
#include "mcu/heap.h"
#include "mcu/heap_backend.h"
#include "mm/slab.h"
#include <assert.h>
#include <string.h>
char _heap_start[256] __attribute__((aligned(8)));
__asm__(".global _heap_end\n.set _heap_end, _heap_start + 256\n");
static unsigned char external[512] __attribute__((aligned(8)));
static size_t external_size;
static unsigned allocations, frees;
size_t arch_mcu_heap_available(void) { return external_size ? 0 : sizeof(external); }
void *arch_mcu_heap_alloc(size_t size) {
 if(external_size || size>sizeof(external))return NULL;
 external_size=size;allocations++;return external;
}
int arch_mcu_heap_owns(const void *p) { return p==external && external_size; }
void arch_mcu_heap_free(void *p) { assert(arch_mcu_heap_owns(p));external_size=0;frees++; }
size_t arch_mcu_heap_allocation_size(const void *p) {
 assert(arch_mcu_heap_owns(p));return external_size;
}
int main(void) {
 mcu_heap_init();mcu_heap_stats_t stats;mcu_heap_get_stats(&stats);
 assert(stats.arena_bytes==256 && stats.free_bytes>200 && stats.free_bytes<256);
 assert(mcu_heap_available()==stats.free_bytes+sizeof(external));
 void *inside=kmalloc(200);assert(inside && !arch_mcu_heap_owns(inside) && !allocations);
 void *outside=kmalloc(300);assert(outside==external && allocations==1);
 memset(outside,0x5a,300);assert(!krealloc(outside,400));
 assert(((unsigned char *)outside)[299]==0x5a && external_size>=300);
 assert(krealloc(outside,100)==outside);
 kfree(outside);assert(frees==1 && !external_size);
 kfree(inside);mcu_heap_get_stats(&stats);
 assert(mcu_heap_available()==stats.free_bytes+sizeof(external));
 assert(!kcalloc(SIZE_MAX,2) && !kmalloc(0));
 outside=kmalloc(400);assert(outside==external && allocations==2);
 kfree(outside);assert(frees==2);
 return 0;
}
'''

class McuHeapBackendTest(unittest.TestCase):
    def test_production_allocator_routes_external_arena(self):
        with tempfile.TemporaryDirectory(prefix='a20-mcu-heap-') as name:
            work = Path(name)
            (work / 'core').mkdir()
            (work / 'core/arch.h').write_text('''#include "core/types.h"
static inline uint32_t arch_irq_save(void) { return 0; }
static inline void arch_irq_restore(uint32_t flags) { (void)flags; }
''')
            (work / 'test.c').write_text(HARNESS)
            executable = work / 'test'
            subprocess.run(['cc', '-std=gnu99', '-ffreestanding', '-Wall', '-Wextra', '-Werror',
                '-fsanitize=undefined', '-fno-sanitize-recover=all',
                '-I'+str(work), '-I'+str(ROOT/'kernel/include'), '-I'+str(ROOT/'kernel'),
                str(work/'test.c'), str(ROOT/'kernel/mcu/heap.c'), '-o', str(executable)], check=True)
            subprocess.run([str(executable)], check=True)

if __name__ == '__main__':
    unittest.main()

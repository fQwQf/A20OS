"""DMA allocation must fail before touching memory on an unready architecture."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_drv_dma_phys_host import extract_function

ROOT = Path(__file__).resolve().parents[2]


class DmaArchitectureReadiness(unittest.TestCase):
    def test_unready_ready_and_absent_arch_hook_execute_production_allocators(self):
        source = (ROOT / 'kernel/drivers/core/driver_hwapi.c').read_text()
        functions = '\n'.join(extract_function(source, signature) for signature in (
            'static int dma_mask_well_formed(', 'static int dma_arch_ready(',
            'int dma_set_mask(', 'uint64_t dma_get_mask(',
            'static int dma_range_ok_mask(', 'void *dma_alloc_coherent(',
            'void *dma_alloc_coherent_aligned('))
        prelude = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define PAGE_SIZE 4096U
#define DMA_MASK_64BIT UINT64_MAX
struct device { uint64_t dma_mask; };
extern int arch_device_dma_ready(void) __attribute__((weak));
static int ready, slab_calls, page_calls;
static unsigned char memory[8192];
void *kmalloc(size_t n) { assert(n<=sizeof(memory)); slab_calls++; return memory; }
void kfree(void *p) { assert(p==memory); }
static uint64_t va_to_pa(const void *p) { assert(p==memory); return 0x88000000; }
static void *dma_alloc_pages(uint64_t mask,size_t size,size_t alignment,uint64_t *handle) {
 assert(mask==0xffffffffULL && size==64 && alignment==64);
 page_calls++;if(handle)*handle=0x88001000;return memory+4096;
}
'''
        main = r'''
int main(void) {
 struct device d={0};uint64_t handle=99;
#ifdef WITH_HOOK
 assert(dma_set_mask(&d,0xffffffffULL)==-EOPNOTSUPP && d.dma_mask==0);
 assert(!dma_alloc_coherent(&d,64,&handle) && handle==0);
 handle=99;assert(!dma_alloc_coherent_aligned(&d,64,64,&handle) && handle==0);
 assert(!slab_calls && !page_calls);
#endif
 ready=1;
 assert(dma_set_mask(&d,0xffffffffULL)==0);
 assert(dma_set_mask(&d,0x1234)==-EINVAL && d.dma_mask==0xffffffffULL);
 assert(dma_alloc_coherent(&d,64,&handle)==memory && handle==0x88000000);
 assert(dma_alloc_coherent_aligned(&d,64,64,&handle)==memory+4096 && handle==0x88001000);
 assert(slab_calls==1 && page_calls==1);
 return 0;
}
'''
        with tempfile.TemporaryDirectory() as directory:
            stage = Path(directory)
            for hook in (False, True):
                with self.subTest(architecture_hook=hook):
                    fixture = prelude + functions
                    if hook:
                        fixture += '\nint arch_device_dma_ready(void) { return ready; }\n'
                    path = stage / 'model.c'
                    path.write_text(fixture + main)
                    binary = stage / 'model'
                    command = ['cc', '-std=c11', '-Wall', '-Wextra', '-Werror',
                               '-fsanitize=undefined', '-fno-sanitize-recover=all']
                    if hook:
                        command.append('-DWITH_HOOK')
                    result = subprocess.run([*command, str(path), '-o', str(binary)],
                                            capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)
                    result = subprocess.run([str(binary)], capture_output=True, text=True)
                    self.assertEqual(result.returncode, 0, result.stderr)


if __name__ == '__main__':
    unittest.main()

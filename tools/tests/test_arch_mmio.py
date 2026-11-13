"""Exercise production architecture mapping hooks against synthetic boot tables.

The callbacks must reject absent/cacheable/wrong-PA aliases before a generic
resource can become a driver pointer. CPU instructions are host stubs only.
"""
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]

CASES = {
 'aarch64': (64, 512, '0', r'''
static uint64_t l1[512] __attribute__((aligned(4096)));
int main(void) {
 boot_pgdir[0]=(uintptr_t)l1|3;
 l1[0]=0x401; l1[1]=0x40000705;
 assert(arch_device_map_mmio(0x9000000,4096,0)==(void *)0x9000000);
 assert(!arch_device_map_mmio(0x40000000,4096,0)); /* Normal RAM */
 assert(!arch_device_map_mmio(0x80000000,4096,0)); /* no descriptor */
 assert(!arch_device_map_mmio(0x3ffff000,8192,0)); /* crosses to RAM */
 l1[0]=0x40000401;
 assert(!arch_device_map_mmio(0x9000000,4096,0)); /* wrong output PA */
 return 0;
}'''),
 'x86_64': (64, 512, '0', r'''
uint64_t boot_pdpt_hh[512];
static uint64_t l3[512] __attribute__((aligned(4096)));
static uint64_t l2[512] __attribute__((aligned(4096)));
int main(void) {
 boot_pgdir[0]=(uintptr_t)l3|3; l3[0]=(uintptr_t)l2|3;
 l2[256]=0x2000009b; l2[257]=0x20200083;
 assert(arch_device_map_mmio(0x20001000,4096,0)==(void *)0x20001000);
 assert(!arch_device_map_mmio(0x20200000,4096,0)); /* WB alias */
 assert(!arch_device_map_mmio(0x20400000,4096,0)); /* absent */
 assert(!arch_device_map_mmio(0x201ff000,8192,0)); /* crosses to WB */
 memset(g_mmio_cache,0,sizeof(g_mmio_cache));
 l2[256]=0x3000009b;
 assert(!arch_device_map_mmio(0x20001000,4096,0)); /* wrong PA */
 return 0;
}'''),
 'arm32': (32, 4096, '0x80000000UL', r'''
int main(void) {
 boot_pgdir[0x81c]=0x01c00616;
 assert(arch_device_map_mmio(0x01c00000,4096,0)==(void *)0x81c00000);
 assert(!arch_device_map_mmio(0x01cfffff,2,0)); /* missing next section */
 boot_pgdir[0x81c]=0x01c0060e;
 assert(!arch_device_map_mmio(0x01c00000,4096,0)); /* cacheable */
 boot_pgdir[0x81c]=0x02c00616;
 assert(!arch_device_map_mmio(0x01c00000,4096,0)); /* wrong PA */
 return 0;
}'''),
 'riscv32': (32, 1024, '0', r'''
int main(void) {
 boot_pgdir[0x340]=(0x10000000 >> 2)|7;
 assert(arch_device_map_mmio(0x10001000,4096,0)==(void *)0xd0001000);
 assert(!arch_device_map_mmio(0x103ff000,8192,0)); /* absent next leaf */
 boot_pgdir[0x340]=(0x10400000 >> 2)|7;
 assert(!arch_device_map_mmio(0x10001000,4096,0)); /* wrong PA */
 assert(!arch_device_map_mmio(0x80000000,4096,0)); /* RAM */
 return 0;
}'''),
 'riscv64': (64, 512, '0xffffffc000000000ULL', r'''
board_stub_t board={0x80000000}; const board_stub_t *current_board=&board;
int main(void) {
 boot_pgdir[256]=7;
 assert(arch_device_map_mmio(0x10001000,4096,0)==(void *)0xffffffc010001000ULL);
 assert(!arch_device_map_mmio(0x3ffff000,8192,0)); /* absent next leaf */
 assert(!arch_device_map_mmio(0x80000000,4096,0)); /* RAM */
 boot_pgdir[256]=1; /* non-leaf, then no writable mapping */
 /* Don't dereference a deliberately invalid next table; absent root suffices. */
 boot_pgdir[256]=0;
 assert(!arch_device_map_mmio(0x10001000,4096,0));
 return 0;
}'''),
}

class ArchMmioTests(unittest.TestCase):
 @unittest.skipUnless(shutil.which('cc'), 'host C compiler required')
 def test_real_boot_mapping_contracts(self):
  for arch,(bits,count,offset,body) in CASES.items():
   with self.subTest(arch=arch), tempfile.TemporaryDirectory() as temp:
    work=Path(temp)
    (work/'platform.h').write_text(f'#include "core/types.h"\n#define PAGE_OFFSET {offset}\n#define PHYS_MEMORY_BASE 0x80000000ULL\nextern uint{bits}_t boot_pgdir[{count}];\n')
    (work/'cpu.h').write_text('#include "core/types.h"\nstatic inline uint64_t rdmsr(uint32_t n) {(void)n;return 0x0007040600070406ULL;}\nstatic inline void wrmsr(uint32_t n,uint64_t v){(void)n;(void)v;}\n')
    (work/'drivers/core').mkdir(parents=True)
    (work/'drivers/core/driver_core.h').write_text('#include "core/types.h"\ntypedef struct {uint64_t ram_base;} board_stub_t;\nextern const board_stub_t *current_board;\n')
    if arch == 'x86_64':
     with (work/'platform.h').open('a') as f:f.write('#define X86_HIGH_RAM_MAP_END (64ULL<<30)\n')
     (work/'core').mkdir()
     (work/'core/lock.h').write_text('#include "core/types.h"\ntypedef int spinlock_t;\n#define SPINLOCK_INIT 0\nstatic inline uint64_t spin_lock_irqsave(spinlock_t*l){assert(!*l);*l=1;return 0;}\nstatic inline void spin_unlock_irqrestore(spinlock_t*l,uint64_t f){(void)f;assert(*l);*l=0;}\n')
     (work/'mm').mkdir()
     (work/'mm/frame.h').write_text('static struct {void *meta;} pfa;\nstatic inline void *frame_alloc_nr(void){return 0;}\n')
     (work/'mm/mm.h').write_text('')
     defines=re.findall(r'^#define PTE_(?:V|W|PS|DEVICE|PAT|NX)\s+.*$',(ROOT/'kernel/arch/x86_64/include/page_table.h').read_text(),re.M)
     (work/'page_table.h').write_text('\n'.join(defines)+'\n')
    source=ROOT/f'kernel/arch/{arch}/platform/device_mmio.c'
    # Production source is included verbatim; synthetic descriptors are inputs.
    (work/'test.c').write_text(f'#include <assert.h>\n#include <string.h>\n#include "{source}"\nuint{bits}_t boot_pgdir[{count}];\n'+body)
    exe=work/'check'
    result=subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror',f'-I{work}',f'-I{ROOT}/kernel/include',f'-I{ROOT}/kernel',str(work/'test.c'),'-o',str(exe)],text=True,capture_output=True)
    self.assertEqual(result.returncode,0,result.stderr)
    subprocess.run([str(exe)],check=True)

if __name__=='__main__': unittest.main()

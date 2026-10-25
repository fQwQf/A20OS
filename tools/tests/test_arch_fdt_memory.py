"""Production ARM/RV32 RAM hooks consume bounded firmware mappings before PFA."""
import json
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_riscv_fdt_memory import dtb

ROOT = Path(__file__).resolve().parents[2]


class ArchitectureRamHooks(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory(prefix='a20-arch-dt-ram-')
        work = Path(cls.tmp.name)
        stubs = {
            'core/panic.h': 'void panic(const char *, ...) __attribute__((noreturn));\n',
            'mm/frame.h': '#define PFA_MAX_RANGES 4\n',
            'drivers/core/driver_core.h': '''#pragma once
#include "core/types.h"
typedef struct { uint64_t ram_base,ram_end; } board_config_t;
extern const board_config_t *const current_board;
''',
            'platform.h': '''#pragma once
#include "core/types.h"
#define PAGE_OFFSET 0UL
#define PHYS_MEMORY_BASE 0x80000000UL
#define PHYS_MEMORY_END 0x90000000UL
#define KERNEL_ENTRY 0x80200000UL
size_t arch_ram_range_count(void);
int arch_ram_range(size_t,paddr_t*,paddr_t*);
''',
        }
        for name,content in stubs.items():
            p=work/name;p.parent.mkdir(parents=True,exist_ok=True);p.write_text(content)
        (work/'host.c').write_text('''
#include <stdio.h>
#include <stdlib.h>
#include "boot/context.h"
#include "drivers/core/driver_core.h"
#include "platform.h"
static boot_context_t context;
const board_config_t *const current_board = NULL;
const boot_context_t *boot_context_get(void) { return &context; }
void panic(const char *reason, ...) { fprintf(stderr,"%s",reason);exit(42); }
int main(int argc,char **argv) {
 if(argc!=2)return 1;
 if(argv[1][0]!='-') {
  FILE *file=fopen(argv[1],"rb");if(!file)return 2;
  void *data=malloc(0x200000);size_t size=fread(data,1,0x200000,file);fclose(file);
  context.fdt=data;context.fdt_available=size;context.fdt_physical=0x8f000000;
 }
 size_t count=arch_ram_range_count();printf("[");
 for(size_t i=0;i<count;i++) {
  paddr_t base,end;if(arch_ram_range(i,&base,&end))return 3;
  printf("%s[%llu,%llu]",i?",":"",(unsigned long long)base,(unsigned long long)end);
 }
 printf("]");return 0;
}
''')
        cls.binaries=[]
        for arch in ('aarch64','arm32','riscv32'):
            binary=work/arch
            subprocess.run(['cc','-std=gnu99','-fno-builtin','-no-pie','-mcmodel=large','-Wall','-Wextra','-Werror',
                '-I',str(work),'-I',str(ROOT/'kernel/include'),'-DCONFIG_RISCV32' if arch=='riscv32' else '-DCONFIG_'+arch.upper(),
                '-Darch_firmware_ram_range_count=arch_ram_range_count','-Darch_firmware_ram_range=arch_ram_range',
                '-D_start=test_kernel_start','-D_end=test_kernel_end',
                '-D_bss_end=test_kernel_end',
                '-Wl,--defsym,test_kernel_start=0x80200000','-Wl,--defsym,test_kernel_end=0x82000000',
                str(work/'host.c'),str(ROOT/f'kernel/arch/{arch}/platform/fdt.c'),
                str(ROOT/'kernel/core/fdt_view.c'),str(ROOT/'kernel/firmware/devicetree/node.c'),
                str(ROOT/'kernel/firmware/devicetree/memory.c'),'-o',str(binary)],check=True)
            cls.binaries.append(binary)
        cls.work=work

    @classmethod
    def tearDownClass(cls): cls.tmp.cleanup()

    def run_hooks(self,blob):
        path=self.work/'case.dtb'
        if blob is not None:path.write_bytes(blob)
        return [subprocess.run([str(p),str(path) if blob is not None else '-'],capture_output=True,text=True) for p in self.binaries]

    def test_absent_firmware_preserves_static_machine_policy(self):
        for result in self.run_hooks(None):
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads(result.stdout),[[0x80000000,0x90000000]])

    def test_reservations_and_dtb_survive_each_architecture(self):
        blob=dtb(child_reservations=[(0x84000000,0x1000)])
        after_dtb=(0x8f000000+len(blob)+4095)&~4095
        for result in self.run_hooks(blob):
            self.assertEqual(result.returncode,0,result.stderr)
            self.assertEqual(json.loads(result.stdout),[[0x80000000,0x84000000],
                [0x84001000,0x8f000000],[after_dtb,0x90000000]])

    def test_capacity_failure_panics_instead_of_dropping_ram(self):
        blob=dtb(child_reservations=[(0x84000000+i*0x10000,4096) for i in range(5)])
        for result in self.run_hooks(blob):self.assertEqual(result.returncode,42)

    def test_present_invalid_firmware_never_uses_static_fallback(self):
        for result in self.run_hooks(dtb()[:-1]):self.assertEqual(result.returncode,42)

if __name__=='__main__': unittest.main()

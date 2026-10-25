"""SSI1 keeps its wire ABI while generic transport treats state as opaque."""
from pathlib import Path
import platform
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]

class SsiArchitectureStateTests(unittest.TestCase):
 @unittest.skipUnless(platform.machine() == 'x86_64' and shutil.which('cc'), 'x86 host compiler required')
 def test_production_wire_validation_and_entry(self):
  with tempfile.TemporaryDirectory() as temp:
   work=Path(temp)
   source=work/'test.c'
   source.write_text(r'''
#include <assert.h>
#include "cluster/arch_state.h"
struct wire {uint64_t regs[18],fs,gs;uint8_t fpu[512] __attribute__((aligned(16)));};
_Static_assert(sizeof(struct wire)==sizeof(cluster_arch_state_t), "SSI1 wire size");
int main(void) {
 cluster_arch_state_t state={0};struct wire *w=(void *)&state;
 w->regs[16]=0x1000;w->regs[7]=0x2000;w->fs=0x3000;w->gs=0x4000;
 assert(arch_cluster_state_valid(&state));
 uint64_t ip,sp,tls;arch_cluster_state_user_entry(&state,&ip,&sp,&tls);
 assert(ip==0x1000 && sp==0x2000 && tls==0x3000);
 w->regs[16]=USER_VA_LIMIT;assert(!arch_cluster_state_valid(&state));w->regs[16]=0x1000;
 w->regs[7]=USER_VA_LIMIT;assert(!arch_cluster_state_valid(&state));w->regs[7]=0x2000;
 w->fs=USER_VA_LIMIT;assert(!arch_cluster_state_valid(&state));w->fs=0x3000;
 w->gs=USER_VA_LIMIT;assert(!arch_cluster_state_valid(&state));w->gs=0x4000;
 *(uint32_t *)(w->fpu+24)=0x10000;assert(!arch_cluster_state_valid(&state));
 return 0;
}
''')
   result=subprocess.run(['cc','-std=gnu99','-Wall','-Wextra','-Werror',
       '-ffreestanding','-fno-builtin','-DCONFIG_X86_64','-DCONFIG_CLUSTER_SSI','-DCONFIG_NR_CPUS=1',
       '-ffunction-sections','-fdata-sections','-Wl,--gc-sections',
       f'-I{ROOT}/kernel/arch/x86_64/include',f'-I{ROOT}/kernel/include',f'-I{ROOT}/kernel',
       str(source),str(ROOT/'kernel/arch/x86_64/cluster/ssi_state.c'),'-o',str(work/'check')],capture_output=True,text=True)
   self.assertEqual(result.returncode,0,result.stderr)
   subprocess.run([str(work/'check')],check=True)

if __name__=='__main__':unittest.main()

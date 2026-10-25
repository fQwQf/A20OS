"""Production LA264 cache handoff plan and exact per-leaf maintenance order."""
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest
ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "kernel/arch/loongarch64/platform/firmware_scanout.c"
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include "kernel/arch/loongarch64/platform/firmware_scanout.h"
static unsigned barrier, calls[2];
static uintptr_t previous[2];
static void drain(void){barrier++;}
static void emit(unsigned op,uintptr_t address){
 assert(op==0x11||op==0x12);unsigned i=op-0x11;
 assert(barrier==i+1);assert(address==0x9000000008000000ULL+64ULL*calls[i]);
 if(calls[i])assert(address==previous[i]+64);
 previous[i]=address;calls[i]++;
}
int main(void){
 uint32_t geom[3]={(6U<<24)|(7U<<16)|3U,(6U<<24)|(7U<<16)|3U,(6U<<24)|(10U<<16)|7U};
 ls2k0300_scanout_plan_t plan;
 assert(!ls2k0300_scanout_plan(0x08000000,8192,0x1d,geom,0x9000000000000011ULL,&plan));
 ls2k0300_scanout_apply(&plan,emit,drain);
 assert(barrier==3&&calls[0]==128&&calls[1]==128);
 assert(!ls2k0300_scanout_plan(0x0efff000,4096,0x7d,geom,0x9000000000000011ULL,&plan));
 assert(ls2k0300_scanout_plan(0x07fff000,4096,0x1d,geom,0x9000000000000011ULL,&plan)==-EINVAL);
 assert(ls2k0300_scanout_plan(0x0efff000,8192,0x1d,geom,0x9000000000000011ULL,&plan)==-EINVAL);
 assert(ls2k0300_scanout_plan(0x08000001,4096,0x1d,geom,0x9000000000000011ULL,&plan)==-EINVAL);
 assert(ls2k0300_scanout_plan(0x08000000,0,0x1d,geom,0x9000000000000011ULL,&plan)==-EINVAL);
 assert(ls2k0300_scanout_plan(UINT64_MAX-4095,8192,0x1d,geom,0x9000000000000011ULL,&plan)==-EINVAL);
 const uint32_t invalid[]={0,0x5,0x1f,0x19,0x9d,0x41d,0x100001d};
 for(unsigned i=0;i<sizeof(invalid)/sizeof(invalid[0]);i++)
  assert(ls2k0300_scanout_plan(0x08000000,4096,invalid[i],geom,0x9000000000000011ULL,&plan)==-EOPNOTSUPP);
 assert(ls2k0300_scanout_plan(0x08000000,4096,0x1d,geom,0x8000000000000001ULL,&plan)==-EOPNOTSUPP);
 for(unsigned i=0;i<3;i++){
  uint32_t old=geom[i];geom[i]^=1U;
  assert(ls2k0300_scanout_plan(0x08000000,4096,0x1d,geom,0x9000000000000011ULL,&plan)==-EOPNOTSUPP);
  geom[i]=old^(1U<<24);
  assert(ls2k0300_scanout_plan(0x08000000,4096,0x1d,geom,0x9000000000000011ULL,&plan)==-EOPNOTSUPP);
  geom[i]=old;
 }
 return 0;
}
'''
class ScanoutHandoff(unittest.TestCase):
    def test_production_plan_and_per_leaf_drains(self):
        with tempfile.TemporaryDirectory(prefix="ls-scanout-") as directory:
            source=Path(directory)/"test.c";binary=Path(directory)/"test"
            source.write_text(HARNESS)
            subprocess.run(["cc","-std=c11","-Wall","-Wextra","-Werror",
                            "-fsanitize=undefined,address","-fno-pie","-no-pie",
                            "-I",str(ROOT),"-I",str(ROOT/"kernel/include"),
                            str(source),"-o",str(binary)],check=True)
            subprocess.run([str(binary)],check=True)

    @unittest.skipUnless(shutil.which("loongarch64-linux-gnu-gcc") and
                         shutil.which("loongarch64-linux-gnu-objdump"),"LoongArch cross tools unavailable")
    def test_actual_cacheop_instructions(self):
        with tempfile.TemporaryDirectory(prefix="ls-scanout-opcodes-") as directory:
            obj=Path(directory)/"scanout.o"
            subprocess.run(["loongarch64-linux-gnu-gcc","-O2","-ffreestanding",
                            "-Wall","-Wextra","-Werror","-DCONFIG_BOARD_LS2K0300",
                            "-I",str(ROOT/"kernel/include"),"-c",str(SOURCE),"-o",str(obj)],check=True)
            assembly=subprocess.check_output(["loongarch64-linux-gnu-objdump","-d",str(obj)],text=True)
            self.assertRegex(assembly,r"cacop\s+0x11,")
            self.assertRegex(assembly,r"cacop\s+0x12,")
            self.assertGreaterEqual(assembly.count("dbar"),3)
            self.assertIn("cpucfg",assembly)
            self.assertIn("0x181",assembly)

if __name__=="__main__":unittest.main()

"""Guest host IRQ dispatch must retain generic hardirq accounting."""
from pathlib import Path
import subprocess
import tempfile
import unittest
ROOT=Path(__file__).resolve().parents[2]
class HypIrqContext(unittest.TestCase):
 def test_host_interrupt_callbacks_cannot_preempt_inside_the_live_guest_frame(self):
  s=(ROOT/'kernel/arch/riscv64/hyp/hyp_arch.c').read_text()
  a=s.index('    if (scause & CAUSE_INTR_MASK) {',s.index('int hyp_arch_guest_trap('));b=s.index('\n    }',a)+len('\n    }')
  body=s[a:b];old=body.replace('        hardirq_enter();\n','')
  self.assertNotEqual(old,body)
  pre=r'''
#include <stdint.h>
#include <assert.h>
#define CAUSE_INTR_MASK (1ULL<<63)
#define CAUSE_CODE_MASK (~CAUSE_INTR_MASK)
static unsigned depth,calls;
static void hardirq_enter(void){depth++;}
static void hardirq_exit(void){assert(depth);depth--;}
static void arch_handle_irq(uint64_t cause,int user){assert(depth&&cause==5&&!user);calls++;}
static void dispatch(uint64_t scause){
'''
  post=r'''
}
int main(void){dispatch(5);assert(!calls&&!depth);dispatch(CAUSE_INTR_MASK|5);assert(calls==1&&!depth);depth=1;dispatch(CAUSE_INTR_MASK|5);assert(calls==2&&depth==1);return 0;}
'''
  with tempfile.TemporaryDirectory(prefix='a20-hyp-hardirq-') as d:
   p=Path(d)
   for name,code in [('fixed',body),('unaccounted',old)]:
    c=p/(name+'.c');c.write_text(pre+code+post);exe=p/name
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Wno-unused-function','-Werror','-fsanitize=undefined',str(c),'-o',str(exe)],check=True)
    r=subprocess.run([str(exe)],capture_output=True)
    if name=='fixed':self.assertEqual(r.returncode,0,r.stderr)
    else:self.assertNotEqual(r.returncode,0)
if __name__=='__main__':unittest.main()

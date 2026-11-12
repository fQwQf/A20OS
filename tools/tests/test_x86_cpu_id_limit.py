"""All configured CPU IDs must use the cached GDT path, including CPUs 8..15."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
class CpuIdLimit(unittest.TestCase):
 def test_decoding_and_previous_eight_cpu_limit(self):
  source=(ROOT/'kernel/arch/x86_64/include/cpu.h').read_text();code=function(source,'x86_cpu_id_from_gdt_limit')
  main=r'''
#include <assert.h>
#define X86_CPU_ID_LIMIT_TAG 0xa2000U
BODY
int main(void){
 for(unsigned i=0;i<CONFIG_NR_CPUS;i++)assert(x86_cpu_id_from_gdt_limit(X86_CPU_ID_LIMIT_TAG|i,1)==i);
 assert(x86_cpu_id_from_gdt_limit(X86_CPU_ID_LIMIT_TAG,0)==CONFIG_NR_CPUS);
 assert(x86_cpu_id_from_gdt_limit(X86_CPU_ID_LIMIT_TAG^0x10000,1)==CONFIG_NR_CPUS);
 assert(x86_cpu_id_from_gdt_limit(X86_CPU_ID_LIMIT_TAG|CONFIG_NR_CPUS,1)==CONFIG_NR_CPUS);
 return 0;
}
'''
  with tempfile.TemporaryDirectory() as d:
   for cpus in (1,4,16,64):
    p=Path(d)/('cpu'+str(cpus)+'.c');exe=p.with_suffix('');p.write_text(main.replace('BODY',code))
    subprocess.run(['cc','-Wall','-Wextra','-Werror','-DCONFIG_NR_CPUS='+str(cpus),str(p),'-o',str(exe)],check=True,capture_output=True)
    subprocess.run([str(exe)],check=True,capture_output=True)
   old=code.replace('valid && id < CONFIG_NR_CPUS','valid && (limit & ~7U) == X86_CPU_ID_LIMIT_TAG')
   self.assertNotEqual(old,code);p.write_text(main.replace('BODY',old))
   subprocess.run(['cc','-DCONFIG_NR_CPUS=16',str(p),'-o',str(exe)],check=True,capture_output=True)
   self.assertNotEqual(subprocess.run([str(exe)],capture_output=True).returncode,0)
if __name__=='__main__':unittest.main()

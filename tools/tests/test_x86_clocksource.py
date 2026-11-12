"""Clock selection must not confuse invariant TSC rate with SMP synchronization."""
from pathlib import Path
import subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <stdint.h>
#include <assert.h>
static unsigned tsc_freq_state,baytrail_tsc_known,use_hpet,mode,probe_calls,hpet_calls;
static uint64_t tsc_freq;
static int hpet_available=1;
static int invariant=1;
static int has_invariant_tsc(void){return invariant;}
static uint64_t discover_tsc_freq(void){probe_calls++;return 3800000000ULL;}
static int enable_hpet(void){hpet_calls++;return hpet_available;}
static void timer_update_clk_scale(void){}
static void cpu_relax(void){}
'''
MAIN=r'''
int main(void){
 for(mode=0;mode<4;mode++){
  tsc_freq_state=baytrail_tsc_known=use_hpet=probe_calls=hpet_calls=0;
  ensure_tsc_freq();assert(tsc_freq==3800000000ULL&&tsc_freq_state==2&&probe_calls==1);
  assert(use_hpet==(CONFIG_NR_CPUS>1));assert(hpet_calls==(CONFIG_NR_CPUS>1));ensure_tsc_freq();assert(probe_calls==1);
 }
 mode=0;tsc_freq_state=use_hpet=0;hpet_available=0;ensure_tsc_freq();assert(!use_hpet);
 tsc_freq_state=use_hpet=hpet_calls=0;baytrail_tsc_known=1;hpet_available=1;
 ensure_tsc_freq();assert(!use_hpet&&!hpet_calls);return 0;
}
'''
class Clocksource(unittest.TestCase):
 def test_invariant_rate_does_not_authorize_smp_tsc(self):
  s=(ROOT/'kernel/arch/x86_64/platform/timer.c').read_text()
  body=function(s,'ensure_tsc_freq')
  with tempfile.TemporaryDirectory() as d:
   for cpus in (1,16):
    p=Path(d)/('clock'+str(cpus)+'.c');exe=p.with_suffix('');p.write_text(PRE+body+MAIN)
    subprocess.run(['cc','-std=gnu11','-DCONFIG_NR_CPUS='+str(cpus),'-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p),'-o',str(exe)],check=True,capture_output=True)
    subprocess.run([str(exe)],check=True,capture_output=True)
 def test_32bit_hpet_wrap_and_tickless_maintenance(self):
  source=(ROOT/'kernel/arch/x86_64/platform/timer.c').read_text()
  code=function(source,'read_hpet_clock')+function(source,'clock_maintenance_interval')
  model=r"""
#include <assert.h>
#include <stdint.h>
#include <pthread.h>
#define HPET_COUNTER 0xf0
#define ARCH_TIMER_FREQ 1000000000ULL
static unsigned hpet_counter_64,use_hpet=1;
static uint64_t hpet_extended_counter,hpet_freq=100000000,raw;
static unsigned hpet_counter_lock,increment;
static __thread int irq=1;
static int arch_irqs_enabled(void){return irq;}
static void arch_local_irq_disable(void){irq=0;}
static void arch_local_irq_enable(void){irq=1;}
static void cpu_relax(void){}
static uint64_t hpet_read(unsigned r){assert(r==HPET_COUNTER);assert(hpet_counter_64||(!irq&&hpet_counter_lock));if(increment)raw=(uint32_t)(raw+1);return raw;}
int main(void);
"""
  main=r"""
static void *reader(void *arg){(void)arg;uint64_t prev=0;
 for(unsigned i=0;i<10000;i++){uint64_t now=read_hpet_clock();assert(now>prev&&irq);prev=now;}
 return 0;
}
int main(void){
 hpet_extended_counter=0xfffffff0ULL;raw=0xfffffff5ULL;assert(read_hpet_clock()==raw);
 raw=0x10;assert(read_hpet_clock()==0x100000010ULL);assert(read_hpet_clock()==0x100000010ULL);
 hpet_extended_counter=0x1fffffff0ULL;raw=5;assert(read_hpet_clock()==0x200000005ULL);
 uint64_t budget=(ARCH_TIMER_FREQ<<31)/hpet_freq;
 assert(clock_maintenance_interval(UINT64_MAX)==budget);
 assert(clock_maintenance_interval(budget-1)==budget-1);
 hpet_counter_64=1;raw=0x9876543210ULL;assert(read_hpet_clock()==raw);
 assert(clock_maintenance_interval(UINT64_MAX)==UINT64_MAX);
 hpet_counter_64=0;hpet_extended_counter=raw=0xffff0000ULL;increment=1;
 pthread_t threads[8];
 for(unsigned i=0;i<8;i++)assert(!pthread_create(&threads[i],0,reader,0));
 for(unsigned i=0;i<8;i++)pthread_join(threads[i],0);
 assert(hpet_extended_counter==0xffff0000ULL+80000&&hpet_counter_lock==0&&irq);
 return 0;
}
"""
  with tempfile.TemporaryDirectory() as d:
   path=Path(d)/'wrap.c';exe=path.with_suffix('');path.write_text(model+code+main)
   subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(path),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()

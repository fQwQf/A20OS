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
static unsigned increment,race;
static uint64_t hpet_read(unsigned r){assert(r==HPET_COUNTER);
 uint64_t value=increment?__atomic_add_fetch(&raw,1,__ATOMIC_RELAXED):raw;
 if(race){__atomic_store_n(&hpet_extended_counter,0x300000020ULL,__ATOMIC_RELEASE);race=0;}
 return hpet_counter_64?value:(uint32_t)value;
}
int main(void);
"""
  main=r"""
static void *reader(void *arg){(void)arg;uint64_t prev=0;
 for(unsigned i=0;i<10000;i++){uint64_t now=read_hpet_clock();assert(now>=prev);prev=now;}
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
 increment=0;assert(read_hpet_clock()==0xffff0000ULL+80000);
 /* A sample delayed past a racing publication must return that winner,
  * rather than misinterpreting its old low bits as an extra wrap. */
 hpet_extended_counter=0x2fffffff0ULL;raw=0x10;race=1;
 assert(read_hpet_clock()==0x300000020ULL);
 assert(hpet_extended_counter==0x300000020ULL);
 /* A preempted reader spanning more than half a period uses maintenance's
  * current epoch rather than its obsolete preemption-time snapshot. */
 hpet_extended_counter=0x100000010ULL;raw=0x90000020;race=1;
 assert(read_hpet_clock()==0x300000020ULL);
 return 0;
}
"""
  with tempfile.TemporaryDirectory() as d:
   path=Path(d)/'wrap.c';exe=path.with_suffix('');path.write_text(model+code+main)
   subprocess.run(['cc','-std=gnu11','-pthread','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(path),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
 def test_normalized_cpu_epochs_and_reference_fallback(self):
  s=(ROOT/'kernel/arch/x86_64/platform/timer.c').read_text()
  model=r"""
#include <assert.h>
#include <stdint.h>
#define CONFIG_NR_CPUS 2
#define ARCH_TIMER_FREQ 1000000000ULL
#define MS_TO_TICKS(x) ((uint64_t)(x)*1000000ULL)
#define KLOG_WARN 1
#define KLOG_INFO 0
#define klog_record_only(...) ((void)0)
struct cpu_clock_anchor {uint64_t cycle,ticks,rate;unsigned ready,verified;};
static struct cpu_clock_anchor cpu_clock[2];
static uint64_t tsc_freq=3800000000ULL,hpet_clock_highwater,cycles[2],reference;
static struct {uint64_t ticks;} last_ticks[2];
static unsigned cpu,irq=1,reads,use_hpet=1;
static unsigned cpu_current_id(void){return cpu;}
static int arch_irqs_enabled(void){return irq;}
static void arch_local_irq_disable(void){irq=0;}
static void arch_local_irq_enable(void){irq=1;}
static uint64_t read_tsc(void){return cycles[cpu];}
static uint64_t read_hpet_clock(void){reads++;return reference;}
static uint64_t scale_ticks_reduced(uint64_t t){return t;}
static void ensure_tsc_freq(void){}
"""
  code='\n'.join(function(s,n) for n in ('scale_ticks','read_reference_clock','refresh_cpu_clock','timer_get_ticks'))
  main=r"""
int main(void){
 cycles[0]=50000000000ULL;cycles[1]=1000000;reference=100000000ULL;
 for(cpu=0;cpu<2;cpu++){irq=0;refresh_cpu_clock(1);assert(cpu_clock[cpu].ready);irq=1;}
 cpu=0;unsigned before=reads;assert(read_reference_clock()==reference&&reads==before+1);
 for(cpu=0;cpu<2;cpu++)cycles[cpu]+=tsc_freq;
 reference+=1000000000ULL;
 for(cpu=0;cpu<2;cpu++){irq=0;refresh_cpu_clock(0);assert(cpu_clock[cpu].verified);irq=1;}
 before=reads;
 for(unsigned i=0;i<100;i++)for(cpu=0;cpu<2;cpu++){
  cycles[cpu]+=tsc_freq/1000;
  assert(read_reference_clock()==1100000000ULL+(i+1)*1000000ULL);assert(irq);
 }
 assert(reads==before); /* No HPET MMIO or global lock on fast reads. */
 /* Simulate small opposite correlation errors, then migrate the caller. */
 cpu_clock[0].ticks+=1000;cpu_clock[1].ticks-=1000;
 cpu=0;uint64_t first=timer_get_ticks();cpu=1;assert(timer_get_ticks()>=first);
 for(cpu=0;cpu<2;cpu++){cycles[cpu]+=tsc_freq;}
 reference=2200000000ULL;
 for(cpu=0;cpu<2;cpu++){irq=0;refresh_cpu_clock(0);assert(cpu_clock[cpu].ready);irq=1;}
 /* An unexpected epoch jump bypasses interpolation and is rejected by the
  * next reference check, without ever publishing the jumped value. */
 cpu=0;cycles[0]+=3*tsc_freq;before=reads;
 assert(read_reference_clock()==reference&&reads==before+1);
 irq=0;refresh_cpu_clock(0);assert(!cpu_clock[0].ready);irq=1;
 assert(read_reference_clock()==reference);
 irq=0;assert(read_reference_clock()==reference&&!irq);
 return 0;
}
"""
  with tempfile.TemporaryDirectory() as d:
   path=Path(d)/'epoch.c';exe=path.with_suffix('');path.write_text(model+code+main)
   subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(path),'-o',str(exe)],check=True,capture_output=True)
   subprocess.run([str(exe)],check=True,capture_output=True)
if __name__=='__main__':unittest.main()

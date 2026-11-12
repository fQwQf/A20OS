"""Scheduler observation must not manufacture protocol-worker wakeups."""
from pathlib import Path
import re,subprocess,tempfile,unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <stdint.h>
#define BSD_MAX_NICS 2
#define BSD_RX_BUDGET 64
#define MS_TO_TICKS(x) ((uint64_t)(x)*1000000ULL)
struct device {int id;};
static struct {void *context;const struct device *device;unsigned rx_pending;} nics[2];
static unsigned progress_budget,clock_due,wakes;
static uint64_t clock_last,now;
static void a20_bsd_executor_notify(void){wakes++;}
static uint64_t timer_get_ticks(void){return now;}
'''
MAIN=r'''
int main(void){
 struct device dev={0};nics[0].context=&dev;nics[0].device=&dev;
 for(unsigned i=0;i<10000;i++){receive_progress(128);protocol_progress(128);assert(!work_pending());}
 assert(wakes==0&&nics[0].rx_pending==0&&progress_budget==128);
 now=MS_TO_TICKS(10);timer_tick();assert(wakes==1&&clock_due==1);
 timer_tick();assert(wakes==1);
 /* Pending work is owned by the ready executor; unrelated idle CPUs sleep. */
 assert(!work_pending());device_interrupt(&dev);assert(wakes==2&&nics[0].rx_pending==1);
 protocol_progress(64);receive_progress(64);assert(wakes==2);
 poll_waiter();assert(wakes==3);
 /* No-IRQ transports still receive bounded periodic executor polling. */
 clock_due=0;nics[0].rx_pending=0;now+=MS_TO_TICKS(20);timer_tick();
 assert(wakes==4&&clock_due==2);return 0;
}
'''
class BsdIdleProgress(unittest.TestCase):
 def test_idle_and_explicit_event_sources(self):
  path='kernel/net/backends/bsd/runtime.c';source=(ROOT/path).read_text()
  funcs=('receive_progress','device_interrupt','protocol_progress','timer_tick','work_pending','poll_waiter')
  def extract(text,n):return re.search(r'^static void poll_waiter[^\n]*',text,re.M).group() if n=='poll_waiter' else function(text,n)
  fixed='\n'.join(extract(source,n) for n in funcs)
  old=fixed.replace(function(source,'protocol_progress'), 'static void protocol_progress(unsigned budget) { (void)budget; a20_bsd_executor_notify(); }')
  with tempfile.TemporaryDirectory() as d:
   def run(body,name):
    p=Path(d)/(name+'.c');exe=p.with_suffix('');p.write_text(PRE+body+MAIN)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(p),'-o',str(exe)],check=True,capture_output=True)
    return subprocess.run([str(exe)],capture_output=True,timeout=5).returncode
   self.assertEqual(run(fixed,'fixed'),0)
   self.assertNotEqual(old,fixed);self.assertNotEqual(run(old,'old'),0)
if __name__=='__main__':unittest.main()

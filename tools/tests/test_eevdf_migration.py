"""Migrate real fair accounting between deliberately divergent CPU clocks."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_pt_mcs_preempt_window import extract_function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <stdint.h>
#include <assert.h>
#define CONFIG_NR_CPUS 2
typedef struct { unsigned cpu_id; uint64_t eevdf_vruntime,eevdf_deadline; int policy; } task_t;
typedef struct { uint64_t eevdf_vtime; } proc_runq_t;
static proc_runq_t sched_runq[2];
static int sched_task_rt(task_t *t){return t->policy==1;}
static int sched_task_dl(task_t *t){return t->policy==2;}
static void eevdf_reset_deadline(task_t *t){t->eevdf_deadline=t->eevdf_vruntime+10;}
extern uint64_t a20_eevdf_rebase(uint64_t,uint64_t,uint64_t);
'''
MAIN=r'''
int main(void){
 sched_runq[0].eevdf_vtime=1000000;sched_runq[1].eevdf_vtime=1000;
 task_t t={.cpu_id=0,.eevdf_vruntime=999990,.eevdf_deadline=1000000};
 proc_sched_set_task_cpu_locked(&t,1);
 if(t.cpu_id!=1||t.eevdf_vruntime!=990||t.eevdf_deadline!=1000)return 33;
 proc_sched_set_task_cpu_locked(&t,0);
 assert(t.eevdf_vruntime==999990&&t.eevdf_deadline==1000000);
 t.eevdf_vruntime=1000020;proc_sched_set_task_cpu_locked(&t,1);
 assert(t.eevdf_vruntime==1020&&t.eevdf_deadline==1030);
 t=(task_t){.cpu_id=0};proc_sched_set_task_cpu_locked(&t,1);
 assert(!t.eevdf_vruntime&&!t.eevdf_deadline); /* first placement */
 t=(task_t){.cpu_id=0,.eevdf_vruntime=100,.eevdf_deadline=110,.policy=1};
 proc_sched_set_task_cpu_locked(&t,1);assert(t.eevdf_vruntime==100&&t.eevdf_deadline==110);
 assert(a20_eevdf_rebase(1,100,5)==0);
 assert(a20_eevdf_rebase(UINT64_MAX,0,5)==UINT64_MAX);
 return 0;
}
'''
class EevdfMigration(unittest.TestCase):
 def test_signed_lag_survives_cpu_clock_changes(self):
  source=(ROOT/'kernel/proc/sched.c').read_text()
  body=extract_function(source,'void proc_sched_set_task_cpu_locked(task_t *t, unsigned destination)')
  with tempfile.TemporaryDirectory(prefix='a20-eevdf-migrate-') as d:
   p=Path(d);obj=p/'math.o'
   subprocess.run(['rustc','--edition=2021','--crate-type=lib','--emit=obj','-C','panic=abort','-C','opt-level=2',str(ROOT/'kernel/proc/eevdf_math.rs'),'-o',str(obj)],check=True)
   old=body.replace('t->eevdf_vruntime = a20_eevdf_rebase(t->eevdf_vruntime, from, to);','(void)from; (void)to;')
   self.assertNotEqual(old,body)
   for name,code,want in [('fixed',body,0),('raw-cpu-clock',old,33)]:
    c=p/(name+'.c');c.write_text(PRE+code+MAIN);binary=p/name
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=undefined',str(c),str(obj),'-o',str(binary)],check=True)
    self.assertEqual(subprocess.run([str(binary)]).returncode,want)
if __name__=='__main__':unittest.main()

"""Compile production kill(-1) dispatch with a referenced task-table adapter."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_usb_hid_readiness import function
ROOT=Path(__file__).resolve().parents[2]
PRE=r'''
#include <assert.h>
#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#define pgid tgid
#define NSIG 65
typedef struct task { int pid,tgid,allow; void *mm,*pgdir; } task_t;
static task_t table[128], self={5,5,1,(void*)1,(void*)1};
static int sent[128], puts, gets;
static task_t *proc_current(void){return &self;}
static task_t *proc_find_get_user(int n){if(n<0||n>=128||!table[n].pid)return NULL;++gets;return &table[n];}
static void proc_put(task_t *t){if(t)++puts;}
static int signal_send_permission(task_t *a,task_t *b){(void)a;return b->allow?0:-EPERM;}
static int proc_kill(int p,int s){assert(s==15);++sent[p];return 0;}
static int proc_kill_pgid(int p,int s,int skip){(void)p;(void)s;(void)skip;return 0;}
static int proc_pid_max(void){return 127;}
'''
MAIN=r'''
int main(void){
 table[1]=(task_t){1,1,1,(void*)1,(void*)1};
 table[2]=(task_t){2,2,1,NULL,(void*)1}; /* kernel worker with page table */
 table[5]=self;table[6]=(task_t){6,5,1,(void*)1,(void*)1};
 table[20]=(task_t){20,20,1,(void*)1,(void*)1};
 table[21]=(task_t){21,20,1,(void*)1,(void*)1};
 table[22]=(task_t){22,20,1,(void*)1,(void*)1};
 table[30]=(task_t){30,30,0,(void*)1,(void*)1};
 /* The caller's namespace number differs from the referenced global pid. */
 table[101]=(task_t){60,60,1,(void*)1,(void*)1};
 assert(sys_kill(-1,15)==0);
 assert(sent[20]==1 && sent[60]==1);
 for(int p=0;p<128;p++)if(p!=20&&p!=60)assert(!sent[p]);
 assert(gets==puts);
 return 0;
}
'''
class KillBroadcast(unittest.TestCase):
 def test_groups_caller_kernel_workers_and_namespace(self):
  code=function((ROOT/'kernel/abi/linux/sys_signal.c').read_text(),'sys_kill')
  old=code.replace('target->pid == tgid && tgid != self_tgid && target->mm',
                   'target->pid > 1 && target->pgdir')
  self.assertNotEqual(code,old)
  with tempfile.TemporaryDirectory() as directory:
   def run(body,name):
    src=Path(directory)/(name+'.c');exe=src.with_suffix('');src.write_text(PRE+body+MAIN)
    subprocess.run(['cc','-std=gnu11','-Wall','-Wextra','-Wno-unused-variable','-Werror','-fsanitize=undefined',str(src),'-o',str(exe)],check=True,capture_output=True)
    return subprocess.run([str(exe)],capture_output=True,timeout=5).returncode
   self.assertEqual(run(code,'correct'),0)
   self.assertNotEqual(run(old,'per-thread'),0)
if __name__=='__main__':unittest.main()

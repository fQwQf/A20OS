"""Execute the production sleep syscalls with explicit Linux wire layouts."""
from pathlib import Path
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <errno.h>
#include <assert.h>
#define _LINUX_SYSCALL_IMPL_H
#define _PROC_INTERNAL_H
#define TICKS_PER_SEC 1000ULL
#define PROC_WAIT_INTERRUPTIBLE 1
#define PROC_WAKE_TIMEOUT_CAPACITY 2
#define PROC_WAKE_SIGNAL 3
#define PROC_WAKE_TIMEOUT 4
typedef int task_t;
typedef int proc_wake_reason_t;
static uint64_t ticks;
static int interrupted, capacity, fromfail, tofail;
static size_t read_bytes, written_bytes;
static task_t current;
static int copy_from_user(void *d,const void*s,size_t n){read_bytes=n;if(fromfail)return -1;memcpy(d,s,n);return 0;}
static int copy_to_user(void*d,const void*s,size_t n){written_bytes=n;if(tofail)return -1;memcpy(d,s,n);return 0;}
static uint64_t timer_get_ticks(void){return ticks;}
static task_t *proc_current(void){return &current;}
static proc_wake_reason_t proc_park_wait(int mode,uint64_t until){(void)mode;if(capacity)return PROC_WAKE_TIMEOUT_CAPACITY;if(interrupted){ticks+=10;return PROC_WAKE_SIGNAL;}ticks=until;return PROC_WAKE_TIMEOUT;}
static int proc_wake_reason_is_task_interrupt(int r){return r==PROC_WAKE_SIGNAL;}
static int signal_task_has_unblocked(task_t*t){(void)t;return 0;}
static void cpu_relax(void){}
static void timekeeping_get_realtime(uint64_t ts[2]){ts[0]=100;ts[1]=0;}
static void timekeeping_get_monotonic(uint64_t ts[2]){ts[0]=ticks/1000;ts[1]=(ticks%1000)*1000000;}
#include "kernel/abi/linux/sys_sleep.c"
int main(void){
 int32_t req32[2]={0,33333333}; int64_t req64[2]={0,33333333};
 assert(!sys_nanosleep_abi(req32,NULL,1)&&read_bytes==8&&ticks==34);
 assert(!sys_clock_nanosleep(1,0,req64,NULL)&&read_bytes==16&&ticks==68);
 req32[0]=-1;assert(sys_nanosleep_abi(req32,NULL,1)==-EINVAL);
 req32[0]=0;req32[1]=-1;assert(sys_nanosleep_abi(req32,NULL,1)==-EINVAL);
 req32[1]=1000000000;assert(sys_nanosleep_abi(req32,NULL,1)==-EINVAL);
 req64[0]=-1;assert(sys_nanosleep(req64,NULL)==-EINVAL);
 req64[0]=0;req64[1]=1000000000;assert(sys_nanosleep(req64,NULL)==-EINVAL);
 struct {int32_t t[2];uint64_t guard;} rem32={{-1,-1},0x1234567812345678ULL};
 int64_t rem64[2]={-1,-1};req32[0]=1;req32[1]=0;req64[0]=1;req64[1]=0;interrupted=1;
 assert(sys_nanosleep_abi(req32,rem32.t,1)==-EINTR);
 assert(written_bytes==8&&rem32.t[0]==0&&rem32.t[1]==990000000&&rem32.guard==0x1234567812345678ULL);
 assert(sys_clock_nanosleep(1,0,req64,rem64)==-EINTR);
 assert(written_bytes==16&&rem64[0]==0&&rem64[1]==990000000);
 written_bytes=0;req32[0]=110;
 assert(sys_clock_nanosleep_abi(0,1,req32,rem32.t,1)==-EINTR&&written_bytes==0);
 tofail=1;assert(sys_nanosleep_abi(req32,rem32.t,1)==-EFAULT);tofail=0;
 fromfail=1;assert(sys_nanosleep_abi(req32,NULL,1)==-EFAULT);fromfail=0;
 interrupted=0;capacity=1;assert(sys_nanosleep_abi(req32,NULL,1)==-EAGAIN);capacity=0;
 assert(sys_clock_nanosleep_abi(0,2,req32,NULL,1)==-EINVAL);
 assert(sys_clock_nanosleep_abi(2,0,req32,NULL,1)==-EOPNOTSUPP);
 req32[0]=99;assert(!sys_clock_nanosleep_abi(0,1,req32,NULL,1));
 req64[0]=INT64_MAX;req64[1]=999999999;
 assert(!sys_clock_nanosleep(1,0,req64,NULL)&&ticks==UINT64_MAX);
 return 0;
}
'''

class SleepABI(unittest.TestCase):
    def test_production_time32_time64_sleep(self):
        with tempfile.TemporaryDirectory(prefix="a20-sleep-") as directory:
            source = Path(directory) / "test.c"
            binary = Path(directory) / "test"
            source.write_text(HARNESS)
            subprocess.run(["cc", "-std=c11", "-Wall", "-Wextra", "-Werror",
                            "-fsanitize=undefined,address", "-fno-pie", "-no-pie",
                            "-I", str(ROOT), "-I", str(ROOT / "kernel"),
                            str(source), "-o", str(binary)], check=True)
            subprocess.run([str(binary)], check=True)

if __name__ == "__main__":
    unittest.main()

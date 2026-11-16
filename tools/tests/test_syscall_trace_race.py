"""Exercise the production tracer with deterministic exit/slot-reuse hooks."""
from pathlib import Path
import re
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
PRE = r'''
#include <assert.h>
#include <errno.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define SPINLOCK_INIT 0
#define TICKS_PER_SEC 100
#define SYS_openat 56
#define SYS_readlinkat 78
#define SYS_statx 291
#define SYS_fstatat 79
#define TRAP_CTX_FP(t) read_trap(t,0)
#define TRAP_CTX_RA(t) read_trap(t,1)
typedef int spinlock_t;
typedef struct {uintptr_t fp,ra;} trap_context_t;
typedef struct {int pid,tgid,refs;char name[64];trap_context_t *trap_ctx;} task_t;
typedef struct {int nr;const char *name;} linux_syscall_entry_t;
typedef struct {uint64_t arg[6];} linux_syscall_args_t;
struct backtrace_frame {uint64_t pc;};
static uint64_t ticks;
static int locked, mode, hooked, slow_lines, backtraces, started;
static task_t a,b;
static trap_context_t context={0,0x1234};
static const linux_syscall_entry_t read_entry={63,"read"},write_entry={64,"write"};
static linux_syscall_args_t arguments={{11,22,33}};
static uint64_t spin_lock_irqsave(spinlock_t *p){assert(!*p&&!locked);*p=1;locked=1;return 0;}
static void spin_unlock_irqrestore(spinlock_t *p,uint64_t f){(void)f;assert(*p&&locked);*p=0;locked=0;}
static uint64_t timer_get_ticks(void){return ticks;}
static const char *bootargs_get(void){return "trace=node";}
static int proc_alloc(void (*fn)(void)){(void)fn;assert(!locked);started++;return 9;}
static task_t *proc_current(void){return &a;}
static void proc_set_name(task_t *t,const char *n){snprintf(t->name,sizeof(t->name),"%s",n);}
static void proc_sleep_until(uint64_t t){(void)t;abort();}
static long user_strncpy(char *dst,const char *src,size_t n){snprintf(dst,n,"%s",src);return 0;}
static task_t *proc_find_get(int pid);
static void proc_put(task_t *t){assert(!locked&&t->refs>0);t->refs--;}
static uint64_t read_trap(trap_context_t *t,int which){assert(!locked&&a.refs>0);return which?t->ra:t->fp;}
static int arch_unwind_frames(uint64_t f,struct backtrace_frame *b,int n){(void)f;(void)b;(void)n;assert(a.refs>0);backtraces++;return 0;}
static void kallsyms_print(uint64_t pc){(void)pc;assert(a.refs>0);}
static int trace_print(const char *fmt,...);
#define printf trace_print
'''
POST = r'''
#undef printf
static task_t *proc_find_get(int pid){
 assert(!locked);
 if(mode==1&&!hooked){
  hooked=1;a.refs++;
  syscall_trace_exit(&a,&read_entry,0);
  return &a;
 }
 if(mode==3&&!hooked){
  hooked=1;
  syscall_trace_exit(&a,&read_entry,0);
  b=(task_t){.pid=a.pid,.tgid=a.pid,.trap_ctx=&context};strcpy(b.name,"node-new");
  ticks=300;syscall_trace_enter(&b,&write_entry,&arguments);
  return NULL;
 }
 if(pid==a.pid){a.refs++;return &a;}
 return NULL;
}
static int trace_print(const char *fmt,...){
 assert(!locked);
 if(strstr(fmt,"[TRACE-SLOW] pid=")){
  assert(a.refs>0);slow_lines++;
  if(mode==2&&!hooked){
   hooked=1;
   syscall_trace_exit(&a,&read_entry,0);
   ticks=300;syscall_trace_enter(&a,&write_entry,&arguments);
  }
 }
 return 0;
}
static void reset(void){
 memset(g_trace_slots,0,sizeof(g_trace_slots));
 memset(g_trace_prefix,0,sizeof(g_trace_prefix));
 g_trace_ready=g_trace_tgid=g_trace_scanner_started=0;
 g_trace_generation=g_trace_lines=0;
 mode=hooked=slow_lines=backtraces=started=0;ticks=100;
 a=(task_t){.pid=7,.tgid=7,.trap_ctx=&context};strcpy(a.name,"node");
 b=(task_t){0};
 syscall_trace_enter(&a,&read_entry,&arguments);
 assert(started==1&&g_trace_slots[0].task==&a);
}
int main(void){
 /* Exit while lookup is pinning: no NULL slot dereference or leaked pin. */
 reset();mode=1;trace_scan_slow_once(200);
 assert(hooked&&!slow_lines&&!a.refs&&!g_trace_slots[0].task);
 /* Exit and reuse during printing: use the old coherent snapshot, retain the
  * task through trap/backtrace reads, and leave the replacement entry intact. */
 reset();mode=2;uint64_t generation=g_trace_slots[0].generation;
 trace_scan_slow_once(200);
 assert(hooked&&slow_lines==1&&backtraces==1&&!a.refs);
 assert(g_trace_slots[0].task==&a&&g_trace_slots[0].entry==&write_entry);
 assert(g_trace_slots[0].generation!=generation);
 /* Failed pid validation must not clear a different task's new slot. */
 reset();mode=3;trace_scan_slow_once(200);
 assert(hooked&&!slow_lines&&g_trace_slots[0].task==&b);
 assert(g_trace_slots[0].entry==&write_entry);
 /* A new entry may start after the scanner sampled its time. */
 reset();trace_scan_slow_once(99);assert(!slow_lines&&!a.refs);
 /* Completing a call still clears its entry after comm/group selection changes. */
 reset();strcpy(a.name,"renamed");g_trace_tgid=999;
 syscall_trace_exit(&a,&read_entry,0);assert(!g_trace_slots[0].task);
 /* Exhausting the output budget must not leak the scanner's task reference. */
 reset();g_trace_lines=TRACE_LINE_BUDGET;trace_scan_slow_once(200);
 assert(!slow_lines&&!a.refs&&g_trace_lines==TRACE_LINE_BUDGET);
 return 0;
}
'''


class SyscallTraceRace(unittest.TestCase):
    def test_exit_reuse_and_reference_lifetime(self):
        source = (ROOT / 'kernel/syscall/trace.c').read_text()
        source = re.sub(r'^#include[^\n]*\n', '', source, flags=re.MULTILINE)
        with tempfile.TemporaryDirectory(prefix='a20-trace-') as directory:
            path = Path(directory) / 'trace.c'
            binary = path.with_suffix('')
            path.write_text(PRE + source + POST)
            subprocess.run(['cc', '-std=gnu11', '-Wall', '-Wextra', '-Werror',
                            '-fsanitize=undefined', '-fno-sanitize-recover=all',
                            str(path), '-o', str(binary)], check=True)
            subprocess.run([str(binary)], check=True, timeout=5)


if __name__ == '__main__':
    unittest.main()

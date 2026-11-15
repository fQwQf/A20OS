"""Real group-control adapter and Rust pending queues: stop, continue and clone."""
from pathlib import Path
import subprocess
import tempfile
import unittest
from tools.tests.test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#define kcalloc calloc
#define kfree free
#define ENOMEM 12
#include "proc/signal_core.h"
#define SIGSTOP 19
#define SIGCONT 18
#define SIGCHLD 17
#define NSIG 64
#define PROC_UNUSED 0
#define PROC_READY 1
#define PROC_STOPPED 5
#define PROC_ZOMBIE 4
typedef struct { int lock; uint64_t pending; uint8_t has_info[64], info[64*128]; } signal_state_t;
typedef struct task {
    signal_state_t *signals; void *pgdir;
    struct task *tg_leader, *tg_next, *parent;
    int park_lock, jobctl_lock; uint32_t jobctl_state; int pid, exit_code, stop_report_pending, continue_report_pending;
    int state, stop_queued, cont_queued, resumes;
    uint64_t thread_pending, jobctl_stop_epoch;
} task_t;
static int tasklist_lock;
static uint64_t spin_lock_irqsave(int *p) { assert(!*p); *p=1; return 0; }
static void spin_unlock_irqrestore(int *p, uint64_t f) { (void)f; assert(*p); *p=0; }
static int proc_task_state_get(task_t *t) { return t->state; }
static uint64_t signal_mask_bit(int sig) { return 1ULL << sig; }
static a20_signal_queue_state_t signal_queue_state_fill(void *p, signal_state_t *ss) {
    task_t *t = p;
    return (a20_signal_queue_state_t){.process_pending=&ss->pending, .thread_pending=&t->thread_pending, .process_has_info=ss->has_info, .process_info=ss->info};
}
static int signal_queue_one(task_t *t, int sig, const void *info, size_t size, int directed) {
    (void)info; (void)size;
    assert(!tasklist_lock && (t->tg_leader ? t->tg_leader : t)->jobctl_lock && !t->signals->lock);
    a20_signal_queue_state_t st = signal_queue_state_fill(t,t->signals);
    a20_signal_queue_result_t result;
    if(sig==SIGSTOP) { t->stop_queued++; t->jobctl_stop_epoch=(t->tg_leader ? t->tg_leader : t)->jobctl_state & ~3u; }
    if(sig==SIGCONT) t->cont_queued++;
    return a20_signal_queue_locked(&st,sig,NULL,0,0,0,directed,0,0,&result);
}
static int proc_sched_resume_stopped(task_t *t, int report) {
    assert(!tasklist_lock && (t->tg_leader ? t->tg_leader : t)->jobctl_lock && !t->signals->lock && report);
    t->resumes++; if(t->state==PROC_STOPPED)t->state=PROC_READY; return 1;
}
'''
NOTIFY = r'''
static unsigned notifications, wakes;
static task_t *proc_get(task_t *t) { return t; }
static void proc_put(task_t *t) { (void)t; }
static void proc_wake_child_waiters(task_t *t) { (void)t; assert(!tasklist_lock); wakes++; }
static int signal_task_sigchld_no_cldstop(task_t *t) { (void)t; return 0; }
static int signal_send(int pid, int sig) { assert(pid==99 && sig==SIGCHLD && !tasklist_lock); notifications++; return 0; }
'''
HARNESS = r'''
int main(void) {
    signal_state_t ss={0};
    task_t parent={.pid=99};
    task_t a={.signals=&ss,.pgdir=(void*)1,.state=PROC_READY};
    task_t b=a, c=a, newborn=a;
    a.parent=&parent;
    a.tg_leader=&a; b.tg_leader=c.tg_leader=newborn.tg_leader=&a;
    a.tg_next=&b; b.tg_next=&c;
    assert(signal_queue_task(&b,SIGSTOP,NULL,0,0)==0);
    task_t *members[]={&a,&b,&c};
    for(unsigned i=0;i<3;i++) {
        assert(members[i]->stop_queued==1);
        assert(members[i]->thread_pending==signal_mask_bit(SIGSTOP));
        assert(!signal_task_group_stop_cancelled(members[i]));

    }
    assert(ss.pending==0 && (a.jobctl_state & 1u));
    a.state=b.state=PROC_STOPPED;
    signal_group_stop_completed(&a,SIGSTOP);
    assert(!notifications && !a.stop_report_pending);
    c.state=PROC_STOPPED;
    signal_group_stop_completed(&c,SIGSTOP);
    signal_group_stop_completed(&a,SIGSTOP);
    assert(notifications==1 && wakes==1 && a.stop_report_pending);
    assert(!b.stop_report_pending && !c.stop_report_pending);
    assert(signal_task_group_stop_active(&c));
    /* CONT races a selected stop whose pending bit was already consumed. */
    a20_signal_queue_state_t st=signal_queue_state_fill(&c,&ss);
    a20_signal_consume_locked(&st,SIGSTOP,NULL,0);
    assert(signal_queue_task(&a,SIGCONT,NULL,0,0)==0);
    assert(a.cont_queued==1 && b.cont_queued==0 && c.cont_queued==0);
    st=signal_queue_state_fill(&a,&ss);
    a20_signal_consume_locked(&st,SIGCONT,NULL,0);
    for(unsigned i=0;i<3;i++) {
        assert(members[i]->state==PROC_READY && members[i]->resumes==1);
        assert(!members[i]->thread_pending);
        assert(signal_task_group_stop_cancelled(members[i]));
    }
    assert(!(a.jobctl_state & 1u) && !ss.pending);
    assert(signal_queue_task(&a,SIGSTOP,NULL,0,1)==0);
    assert(!signal_task_group_stop_cancelled(&c));
    tasklist_lock=1;
    signal_group_stop_inherit(&newborn);
    tasklist_lock=0;
    assert(newborn.thread_pending==signal_mask_bit(SIGSTOP));
    assert(!signal_task_group_stop_cancelled(&newborn));
    assert(!tasklist_lock && !ss.lock);
    /* A large group takes the reference-snapshot allocation/retry path. */
    task_t big[33] = {0};
    signal_state_t copied = {0};
    for(unsigned i=0;i<33;i++) {
        big[i].signals = i==1 ? &copied : &ss;
        big[i].pgdir = (void*)1; big[i].state=PROC_READY;
        big[i].tg_leader=&big[0];
        big[i].tg_next=i<32 ? &big[i+1] : NULL;
    }
    assert(!signal_queue_task(&big[0],SIGSTOP,NULL,0,0));
    for(unsigned i=0;i<33;i++) {
        assert(big[i].stop_queued==1 && big[i].thread_pending==signal_mask_bit(SIGSTOP));
        big[i].state=PROC_STOPPED;
    }
    assert(!signal_queue_task(&big[1],SIGCONT,NULL,0,0));
    for(unsigned i=0;i<33;i++) {
        assert(!big[i].thread_pending && big[i].state==PROC_READY);
        assert(signal_task_group_stop_cancelled(&big[i]));
    }
    assert(big[1].cont_queued==1 && big[0].cont_queued==0);
    return 0;
}
'''


class SignalGroupControl(unittest.TestCase):
    def test_all_members_stop_and_continue_does_not_duplicate_handlers(self):
        source = (ROOT / 'kernel/proc/signal.c').read_text()
        bodies = '\n'.join(extract_function(source, signature) for signature in (
            'static int signal_queue_task(task_t *t, int signum, const void *info,',
            'void signal_group_stop_inherit(void *task)',
            'int signal_task_group_stop_cancelled(void *task)',
            'int signal_task_group_stop_active(void *task)',
            'void signal_group_stop_completed(void *task, int sig)',
        ))
        with tempfile.TemporaryDirectory(prefix='a20-group-control-') as d:
            path = Path(d)
            (path / 'core').mkdir()
            (path / 'core/types.h').write_text('#include <stdint.h>\n#include <stddef.h>\n')
            obj = path / 'signal.o'
            subprocess.run(['rustc', '--edition=2021', '--crate-type=lib', '--emit=obj',
                            '-C', 'panic=abort', '-C', 'opt-level=2',
                            str(ROOT / 'kernel/proc/signal_core.rs'), '-o', str(obj)], check=True)
            (path / 'main.c').write_text(PRELUDE + NOTIFY + bodies + HARNESS)
            subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                            '-I', str(path), '-I', str(ROOT / 'kernel'),
                            str(path / 'main.c'), str(obj), '-Wl,--gc-sections',
                            '-o', str(path / 'test')], check=True)
            subprocess.run([str(path / 'test')], check=True)


if __name__ == '__main__':
    unittest.main()

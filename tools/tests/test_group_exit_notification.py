#!/usr/bin/env python3
"""Actual group-dead/notification bodies: signal parents only when reportable."""
from pathlib import Path
import subprocess
import tempfile
from test_pt_mcs_preempt_window import extract_function

ROOT = Path(__file__).resolve().parents[2]
PRELUDE = r'''
#include <assert.h>
#include <stddef.h>
#define PROC_UNUSED 0
#define PROC_RUNNING 2
#define PROC_ZOMBIE 5
struct task {
    int state, tgid, exit_parent_notified, exit_signal, refs, auto_reap;
    struct task *tg_leader, *tg_next, *parent;
};
typedef struct task task_t;
static task_t idle;
static int list_locked=1, get_calls;
static task_t *proc_idle_task(void) { return &idle; }
static int proc_task_state_get(task_t *t) { assert(list_locked); return t->state; }
static int proc_task_tgid(task_t *t) { return t->tgid; }
static int proc_child_auto_reaps(task_t *t, task_t *p) {
    assert(list_locked && p == t->parent); return t->auto_reap;
}
static task_t *proc_get(task_t *t) {
    assert(list_locked); get_calls++;
    if (!t->refs) return NULL;
    t->refs++; return t;
}
'''
HARNESS = r'''
int main(void) {
    task_t parent={.state=PROC_RUNNING,.refs=1};
    task_t leader={.state=PROC_RUNNING,.tgid=6,.exit_signal=17,.parent=&parent};
    task_t sibling={.state=PROC_RUNNING,.tgid=6};
    leader.tg_leader=&leader; leader.tg_next=&sibling; sibling.tg_leader=&leader;
    int signal=999;
    assert(!proc_group_exit_parent_get_locked(NULL,&signal));
    assert(!proc_group_exit_parent_get_locked(&leader,&signal));
    leader.state=PROC_ZOMBIE;
    /* A WNOHANG signal handler still sees no reportable process here. */
    assert(!proc_group_exit_parent_get_locked(&leader,&signal));
    assert(!leader.exit_parent_notified && signal==999 && !get_calls);
    sibling.state=PROC_ZOMBIE;
    assert(proc_group_exit_parent_get_locked(&leader,&signal)==&parent);
    assert(signal==17 && leader.exit_parent_notified && parent.refs==2 && get_calls==1);
    /* Concurrent exits serialized by tasklist_lock must not signal twice. */
    assert(!proc_group_exit_parent_get_locked(&leader,&signal));
    assert(parent.refs==2 && get_calls==1);
    parent.refs--;
    for (int signo=0; signo<2; signo++) {
        leader.exit_parent_notified=0; leader.exit_signal=signo ? 12 : 0;
        assert(proc_group_exit_parent_get_locked(&leader,&signal)==&parent);
        assert(signal==leader.exit_signal); parent.refs--;
    }
    leader.exit_parent_notified=0; leader.auto_reap=1;
    assert(!proc_group_exit_parent_get_locked(&leader,&signal));
    leader.auto_reap=0; leader.parent=&idle;
    assert(!proc_group_exit_parent_get_locked(&leader,&signal));
    leader.parent=NULL;
    assert(!proc_group_exit_parent_get_locked(&leader,&signal));
    leader.parent=&parent; parent.refs=0;
    assert(!proc_group_exit_parent_get_locked(&leader,&signal));
    assert(!leader.exit_parent_notified);
    return 0;
}
'''


def main():
    source = (ROOT / 'kernel/proc/exit.c').read_text()
    bodies = '\n'.join(extract_function(source, signature) for signature in [
        'int proc_tg_group_dead_locked(task_t *t)',
        'static task_t *proc_group_exit_parent_get_locked(',
    ])
    with tempfile.TemporaryDirectory(prefix='a20-group-exit-') as directory:
        directory = Path(directory)
        src, exe = directory / 'group.c', directory / 'group'
        src.write_text(PRELUDE + bodies + HARNESS)
        subprocess.run(['cc', '-std=c11', '-O2', '-Wall', '-Wextra', '-Werror',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)
    print('test-group-exit-notification: PASS (real bodies, group state, once-only, parent refs, ignored parent)')


if __name__ == '__main__':
    main()

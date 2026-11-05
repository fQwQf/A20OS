#!/usr/bin/env python3
"""Run real switch completion and reap-ready notification with adversarial stubs.

Tests both completion entry points, a competing reaper between CPU release and
notification, retained group anchors, notification deduplication and lock order.
"""
from pathlib import Path
import subprocess
import tempfile
from test_anon_final_destroy_host import extract_function

ROOT = Path(__file__).resolve().parents[2]
HARNESS = r'''
#include <assert.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_NR_CPUS 2
#define CONFIG_DEBUG_SCHED_STATE 0
#define PROC_CPU_NONE 99
#define PROC_READY 1
#define PROC_RUNNING 2
#define PROC_ZOMBIE 5
#define PROC_UNUSED 0
typedef struct { int kind; } spinlock_t;
typedef struct task task_t;
struct task {
    int on_cpu, state, dispatching, on_rq, affinity_migrate_pending;
    unsigned owner_cpu, cpu_id;
    void *mm;
    spinlock_t park_lock;
    int refs, freed, wakes, dead_group;
    task_t *parent, *tg_leader;
};
static task_t outgoing, running, parent, group_parent, leader, idle;
static task_t *g_cpu_switching_out[2];
static spinlock_t g_cpu_switch_out[2] = {{1}, {1}};
static spinlock_t tasklist_lock = {3};
static int switch_held, park_held, list_held, inject_reaper, zombie_notes;
static unsigned cpu_current_id(void) { return 0; }
static task_t *proc_current_on_cpu(unsigned cpu) { (void)cpu; return &running; }
static task_t *proc_get(task_t *t) {
    if (t) { assert(t->refs > 0 && !t->freed); t->refs++; }
    return t;
}
static void proc_put(task_t *t) {
    if (!t) return;
    assert(t->refs > 0 && !t->freed);
    if (--t->refs == 0) {
        assert(!switch_held && !park_held && !list_held);
        t->freed = 1;
    }
}
static uint64_t spin_lock_irqsave(spinlock_t *lock) {
    if (lock->kind == 1) { assert(!switch_held && !park_held && !list_held); switch_held++; }
    else if (lock->kind == 2) { assert(!park_held); park_held++; }
    else { assert(!switch_held && !park_held && !list_held); list_held++; }
    return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    if (lock->kind == 1) {
        assert(switch_held == 1 && !park_held); switch_held--;
        if (inject_reaper) {
            inject_reaper = 0;
            assert(!outgoing.on_cpu && g_cpu_switching_out[0] == NULL);
            outgoing.state = PROC_UNUSED;
            outgoing.parent = (task_t *)(uintptr_t)1; /* stale after detach */
            proc_put(&outgoing); /* competing reaper releases allocation ref */
            assert(!outgoing.freed); /* completion must retain a task pin */
        }
    } else if (lock->kind == 2) { assert(park_held == 1); park_held--; }
    else { assert(list_held == 1 && !park_held); list_held--; }
}
static void mm_context_leave(void *mm, unsigned cpu) { (void)mm; (void)cpu; }
static unsigned proc_sched_select_cpu_locked(task_t *t) { (void)t; return 0; }
static void proc_runq_enqueue_locked(task_t *t) { t->on_rq = 1; }
static int proc_sched_should_preempt_locked(task_t *t, unsigned cpu) { (void)t; (void)cpu; return 0; }
static void proc_sched_request_cpu(unsigned cpu, int resched) { (void)cpu; (void)resched; }
static void proc_sched_assert_task_locked(task_t *t) { assert(!t->freed); }
static void proc_sched_note_zombie(void) { assert(!switch_held && !park_held); zombie_notes++; }
static int proc_task_state_get(task_t *t) {
    assert(list_held && !park_held && !t->freed); return t->state;
}
static task_t *proc_idle_task(void) { return &idle; }
static int proc_tg_group_dead_locked(task_t *t) { assert(list_held); return t->dead_group; }
static void proc_wake_child_waiters(task_t *t) {
    assert(!switch_held && !park_held && !list_held);
    if (t) { assert(!t->freed && t->refs > 1); t->wakes++; }
}
void proc_notify_reap_ready(task_t *task);
/* PRODUCTION */
static void reset(void) {
    assert(!switch_held && !park_held && !list_held);
    memset(&outgoing, 0, sizeof(outgoing));
    memset(&parent, 0, sizeof(parent));
    memset(&group_parent, 0, sizeof(group_parent));
    memset(&leader, 0, sizeof(leader));
    parent.refs = group_parent.refs = leader.refs = 1;
    outgoing.refs = 2; /* CPU + task-list allocation */
    outgoing.state = PROC_ZOMBIE;
    outgoing.on_cpu = 1; outgoing.owner_cpu = 0; outgoing.park_lock.kind = 2;
    outgoing.parent = &parent; outgoing.tg_leader = &outgoing;
    g_cpu_switching_out[0] = &outgoing;
    zombie_notes = inject_reaper = 0;
}
int main(void) {
    void (*complete[])(void) = { proc_switch_complete, proc_switch_out_finish_pending };
    for (size_t i = 0; i < 2; i++) {
        reset(); complete[i]();
        assert(parent.wakes == 1 && parent.refs == 1 && outgoing.refs == 1);
        assert(!outgoing.on_cpu && outgoing.owner_cpu == PROC_CPU_NONE && zombie_notes == 1);
        complete[i](); /* no stale duplicate notification */
        assert(parent.wakes == 1 && outgoing.refs == 1 && zombie_notes == 1);

        reset(); inject_reaper = 1; complete[i]();
        assert(outgoing.freed && parent.wakes == 0 && zombie_notes == 1);

        reset(); outgoing.tg_leader = &leader;
        leader.state = PROC_ZOMBIE; leader.dead_group = 1; leader.parent = &group_parent;
        inject_reaper = 1; complete[i]();
        assert(outgoing.freed && parent.wakes == 0 && group_parent.wakes == 1);
        assert(group_parent.refs == 1 && leader.refs == 1);

        reset(); outgoing.tg_leader = &leader;
        leader.state = PROC_ZOMBIE; leader.dead_group = 1; leader.parent = &parent;
        complete[i](); assert(parent.wakes == 1 && parent.refs == 1);

        reset(); outgoing.tg_leader = &leader;
        leader.state = PROC_UNUSED; leader.parent = (task_t *)(uintptr_t)1;
        complete[i](); assert(parent.wakes == 1 && group_parent.wakes == 0);

        reset(); outgoing.state = PROC_READY; complete[i]();
        assert(outgoing.on_rq && outgoing.refs == 1 && !parent.wakes && !zombie_notes);
    }
    puts("reap-ready-host: PASS completion pins, competing reap, group wake, lock order");
    return 0;
}
'''

def main():
    current = (ROOT / 'kernel/proc/current.c').read_text()
    exit_source = (ROOT / 'kernel/proc/exit.c').read_text()
    code = '\n'.join(extract_function(current, sig) for sig in (
        'static task_t *proc_switch_complete_locked(',
        'void proc_switch_out_finish_pending(', 'void proc_switch_complete('))
    code += '\n' + extract_function(exit_source, 'void proc_notify_reap_ready(')
    with tempfile.TemporaryDirectory(prefix='a20-reap-ready-') as tmp:
        src, exe = Path(tmp)/'test.c', Path(tmp)/'test'
        src.write_text(HARNESS.replace('/* PRODUCTION */', code))
        subprocess.run(['cc', '-std=gnu17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()

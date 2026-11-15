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
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#define CONFIG_NR_CPUS 2
#define CONFIG_ABI_BOTH 1
#define PROC_GROUP_EXIT_NOTIFY_ENABLED 1
#define A20_TASK_OBJECTS_ENABLED 1
#define A20_EVENT_EXITED 8
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
    int reap_wait_requested;
    task_t *exit_signal_parent, *exit_signal_child;
    int exit_signal_pending, signals;
    int exit_events_pending, exit_code, events;
    task_t *parent, *tg_leader;
};
static task_t outgoing, running, parent, group_parent, leader, idle;
static task_t *g_cpu_switching_out[2];
static spinlock_t g_cpu_switch_out[2] = {{1}, {1}};
static spinlock_t tasklist_lock = {3};
static int switch_held, park_held, list_held, inject_reaper, zombie_notes;
static unsigned mock_cpu;
static unsigned cpu_current_id(void) { return mock_cpu; }
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
static void proc_sched_set_task_cpu_locked(task_t *t, unsigned cpu) { t->cpu_id = cpu; }
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
typedef struct { int options; } wait4_adapter_t;
#define WNOHANG 1
static int proc_task_is_current_any_cpu(task_t *t) {
    assert(park_held);
    return g_cpu_switching_out[0] == t || g_cpu_switching_out[1] == t;
}
static void proc_wake_child_waiters(task_t *t) {
    assert(!switch_held && !park_held && !list_held);
    if (t) { assert(!t->freed && t->refs > 1); t->wakes++; }
}
typedef struct {
    task_t *task, *parent, *completed_parent, *notify_leader;
    int completed_signal;
} task_exit_context_t;
static task_exit_context_t *exit_ctx(void *p) { return p; }
static void signal_send_task(task_t *p, int sig) {
    assert(!switch_held && !park_held && !list_held);
    assert(p->refs > 1 && !p->freed && sig > 0); p->signals++;
}
static void signal_notify_child_exit(void *p, void *c) {
    task_t *child = c;
    assert(!child->on_cpu && !child->freed && child->refs > 0);
    signal_send_task(p, 17);
}
static void a20_task_object_notify(task_t *t, int event, uint64_t code, int unused, int closing) {
    (void)code; (void)unused;
    assert(!t->on_cpu && !t->freed && t->refs > 0 && closing && event == A20_EVENT_EXITED);
    assert(!switch_held && !park_held && !list_held); t->events++;
}
typedef struct { task_t *task; int pid; } pidfd_file_t;
typedef struct { void *priv; } vfile_t;
#define POLLNVAL 32
static void *a20_pidfd_state_task_rust(void *const *p) { return *p; }
static int a20_pidfd_group_exited_rust(bool found, bool unused, bool zombie, bool dead) {
    return found && (unused || (zombie && dead));
}
static int a20_pidfd_poll_events_rust(bool exited, int events) { return exited ? events : 0; }
void proc_finish_exit_signal(task_t *task);
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
    outgoing.reap_wait_requested = 1;
    outgoing.on_cpu = 1; outgoing.owner_cpu = 0; outgoing.park_lock.kind = 2;
    outgoing.parent = &parent; outgoing.tg_leader = &outgoing;
    g_cpu_switching_out[0] = &outgoing;
    zombie_notes = inject_reaper = 0; mock_cpu = 0;
    g_cpu_switching_out[1] = NULL; leader.park_lock.kind = 2;
}
int main(void) {
    void (*complete[])(void) = { proc_switch_complete, proc_switch_out_finish_pending };
    wait4_adapter_t waiter = {0};
    for (size_t i = 0; i < 2; i++) {
        /* Readability guarantees that a subsequent WNOHANG can reap. */
        reset(); outgoing.dead_group = 1;
        pidfd_file_t pf = { .task = proc_get(&outgoing) }; vfile_t vf = { .priv = &pf };
        assert(pidfd_poll(&vf, 1) == 0);
        complete[i](); assert(pidfd_poll(&vf, 1) == 1); proc_put(pf.task);

        /* Registration wins CPU release: completion owes a notification. */
        reset(); outgoing.reap_wait_requested = 0;
        uint64_t lf = spin_lock_irqsave(&tasklist_lock);
        assert(wait4_adapter_is_current_any_cpu(&waiter, &outgoing));
        assert(outgoing.reap_wait_requested);
        spin_unlock_irqrestore(&tasklist_lock, lf);
        complete[i](); assert(parent.wakes == 1);

        /* CPU release wins: the subsequent scan can reap without parking. */
        reset(); outgoing.reap_wait_requested = 0; complete[i]();
        lf = spin_lock_irqsave(&tasklist_lock);
        assert(!wait4_adapter_is_current_any_cpu(&waiter, &outgoing));
        assert(!outgoing.reap_wait_requested && parent.wakes == 0);
        spin_unlock_irqrestore(&tasklist_lock, lf);

        /* WNOHANG never registers an unnecessary completion wake. */
        reset(); outgoing.reap_wait_requested = 0; waiter.options = WNOHANG;
        lf = spin_lock_irqsave(&tasklist_lock);
        assert(wait4_adapter_is_current_any_cpu(&waiter, &outgoing));
        assert(!outgoing.reap_wait_requested);
        spin_unlock_irqrestore(&tasklist_lock, lf);
        complete[i](); assert(parent.wakes == 0); waiter.options = 0;

        reset(); complete[i]();
        assert(parent.wakes == 1 && parent.refs == 1 && outgoing.refs == 1);
        assert(!outgoing.on_cpu && outgoing.owner_cpu == PROC_CPU_NONE && zombie_notes == 1);
        complete[i](); /* no stale duplicate notification */
        assert(parent.wakes == 1 && outgoing.refs == 1 && zombie_notes == 1);

        reset(); inject_reaper = 1; complete[i]();
        assert(outgoing.freed && parent.wakes == 0 && zombie_notes == 1);

        reset(); outgoing.reap_wait_requested = 0; complete[i]();
        assert(parent.wakes == 0 && outgoing.refs == 1 && zombie_notes == 1);

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

        /* Exactly one deferred signal, even if another CPU already reaped. */
        for (int compete = 0; compete < 2; compete++) {
            reset(); outgoing.reap_wait_requested = 0;
            outgoing.exit_events_pending = 1;
            task_exit_context_t ctx = { .task = &outgoing,
                .parent = proc_get(&parent), .completed_parent = proc_get(&parent),
                .notify_leader = proc_get(&outgoing), .completed_signal = 17 };
            exit_notify_parent(&ctx);
            assert(parent.signals == 0 && parent.refs == 2);
            inject_reaper = compete; complete[i]();
            assert(parent.signals == 1 && parent.refs == 1);
            assert(outgoing.events == 1);
            assert(outgoing.refs == 1 - compete);
            complete[i](); assert(parent.signals == 1);
            assert(outgoing.events == 1);
        }

        /* Last sibling leaves first: hand ownership to its on-CPU leader. */
        reset(); outgoing.reap_wait_requested = 0;
        outgoing.tg_leader = &leader;
        leader.state = PROC_ZOMBIE; leader.on_cpu = 1; leader.owner_cpu = 1;
        leader.tg_leader = &leader; leader.parent = &group_parent;
        leader.refs++; g_cpu_switching_out[1] = &leader;
        task_exit_context_t ctx = { .task = &outgoing,
            .parent = proc_get(&parent), .completed_parent = proc_get(&group_parent),
            .notify_leader = proc_get(&leader), .completed_signal = 17 };
        exit_notify_parent(&ctx); complete[i]();
        assert(group_parent.signals == 0 && group_parent.refs == 2 && leader.refs == 3);
        assert(leader.exit_signal_child == &leader);
        mock_cpu = 1; complete[i]();
        assert(group_parent.signals == 1 && group_parent.refs == 1 && leader.refs == 1);
        complete[i](); assert(group_parent.signals == 1);
        mock_cpu = 0;

        reset(); outgoing.state = PROC_READY; complete[i]();
        assert(outgoing.on_rq && outgoing.refs == 1 && !parent.wakes && !zombie_notes);
    }
    puts("reap-ready-host: PASS completion pins, competing reap, group wake, lock order, deferred SIGCHLD/native EXITED, leader handoff, pidfd readiness");
    return 0;
}
'''

def main():
    current = (ROOT / 'kernel/proc/current.c').read_text()
    exit_source = (ROOT / 'kernel/proc/exit.c').read_text()
    code = '\n'.join(extract_function(current, sig) for sig in (
        'static task_t *proc_switch_complete_locked(',
        'void proc_switch_out_finish_pending(', 'void proc_switch_complete('))
    code += '\n' + '\n'.join(extract_function(exit_source, sig) for sig in (
        'void proc_notify_reap_ready(', 'void proc_finish_exit_signal(',
        'static void exit_notify_parent('))
    code += '\n' + extract_function((ROOT / 'kernel/proc/wait.c').read_text(),
                                    'static int wait4_adapter_is_current_any_cpu(')
    code += '\n' + extract_function((ROOT / 'kernel/abi/linux/sys_pidfd.c').read_text(), 'static int pidfd_poll(')
    with tempfile.TemporaryDirectory(prefix='a20-reap-ready-') as tmp:
        src, exe = Path(tmp)/'test.c', Path(tmp)/'test'
        src.write_text(HARNESS.replace('/* PRODUCTION */', code))
        subprocess.run(['cc', '-std=gnu17', '-Wall', '-Wextra', '-Werror',
                        '-fsanitize=address,undefined', '-fno-omit-frame-pointer',
                        str(src), '-o', str(exe)], check=True)
        subprocess.run([str(exe)], check=True)

if __name__ == '__main__':
    main()

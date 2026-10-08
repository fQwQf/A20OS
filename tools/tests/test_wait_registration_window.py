#!/usr/bin/env python3
"""Exercise the real proc_wait4 body across its scan-to-park race window.

The harness injects a child exit immediately after wait4 drops tasklist_lock
and before it registers the waiter. A correct implementation must not sleep
past that event: it either observes a queued wake or rescans and reaps the
child. The current implementation should fail deterministically by entering
proc_park_commit without a wake queued.
"""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
WAIT_SOURCE = ROOT / "kernel/proc/wait.c"


def extract_function(source: str, signature: str) -> str:
    start = source.index(signature)
    brace = source.index("{", start)
    depth = 0
    for end in range(brace, len(source)):
        if source[end] == "{":
            depth += 1
        elif source[end] == "}":
            depth -= 1
            if depth == 0:
                return source[start : end + 1]
    raise ValueError(f"unterminated function: {signature}")


PRELUDE = r"""
#include <setjmp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define WNOHANG 1
#define WUNTRACED 2
#define WCONTINUED 8
#ifndef WNOWAIT
#define WNOWAIT 0x1000000
#endif
#define __WNOTHREAD 0x20000000
#define ECHILD 10
#define ERESTARTSYS 512
#define PROC_UNUSED 0
#define PROC_RUNNABLE 1
#define PROC_RUNNING 2
#define PROC_ZOMBIE 3
#define PROC_STOPPED 4
#define PROC_WAIT_INTERRUPTIBLE 5
#define PROC_WAKE_SIGNAL 1
#define PROC_WAKE_TASK 2
#define SIGTRAP 5

typedef struct spinlock { int id; } spinlock_t;
typedef struct task task_t;
struct task {
    int pid, ppid, tgid, pgid, state, exit_code;
    int stop_report_pending, continue_report_pending;
    int ptrace_stop_active, ptrace_event;
    int waiting_for_child;
    uint64_t total_time, stime_ticks, child_utime, child_stime;
    task_t *parent, *tg_leader, *tg_next, *children, *sibling_next;
    spinlock_t park_lock;
};
typedef struct { unsigned seq; } proc_wait_token_t;
typedef int proc_wake_reason_t;

static spinlock_t tasklist_lock = {1};
static task_t waiter, child, live_member;
static unsigned g_proc_waiting_child_waiter_count;
static int injected, wake_queued, parked_without_wake;
static int live_group_test;
static jmp_buf parked;

static uint64_t spin_lock_irqsave(spinlock_t *lock) { (void)lock; return 0; }
static void inject_child_exit(void);
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    if (lock == &tasklist_lock && !injected)
        inject_child_exit();
}
static void inject_child_exit(void) {
    injected = 1;
    child.state = PROC_ZOMBIE;
    /* The production notifier only sees registered waiters. */
    if (waiter.waiting_for_child)
        wake_queued = 1;
}
static task_t *proc_current(void) { return &waiter; }
static int proc_task_state_get(task_t *t) { return t->state; }
static int proc_tg_group_dead_locked(task_t *t) {
    if (!live_group_test)
        return 1;
    for (task_t *m = t->tg_next; m; m = m->tg_next) {
        if (m->tgid == t->tgid && m->state != PROC_UNUSED &&
            m->state != PROC_ZOMBIE)
            return 0;
    }
    return 1;
}
static int proc_task_is_current_any_cpu(task_t *t) { (void)t; return 0; }
static task_t *proc_get(task_t *t) { return t; }
static void proc_reap_detach_list_locked(task_t *t) { (void)t; }
static void proc_destroy_task(task_t *t) { (void)t; }
static void proc_put(task_t *t) { (void)t; }
static void proc_park_finish(proc_wait_token_t token) { (void)token; }
static proc_wait_token_t proc_park_prepare_locked(int state, int timeout) {
    (void)state; (void)timeout;
    return (proc_wait_token_t){.seq = 1};
}
static int signal_task_has_unblocked(task_t *t) { (void)t; return 0; }
static int proc_try_wake_locked(task_t *t, unsigned seq, int why) {
    (void)t; (void)seq; (void)why; return 0;
}
static proc_wake_reason_t proc_park_commit(proc_wait_token_t token) {
    (void)token;
    if (!wake_queued) {
        parked_without_wake = 1;
        longjmp(parked, 1);
    }
    return PROC_WAKE_TASK;
}
static int proc_wake_reason_is_task_interrupt(proc_wake_reason_t reason) {
    (void)reason; return 0;
}
static void proc_yield(void) { }
"""


HARNESS = r"""
int main(void) {
    waiter = (task_t){.pid=10, .tgid=10, .pgid=10};
    child = (task_t){.pid=11, .ppid=10, .tgid=11, .pgid=10,
                     .state=PROC_RUNNING, .parent=&waiter};
    waiter.tg_leader = &waiter;
    waiter.children = &child;

    if (setjmp(parked) == 0) {
        int status = 0;
        int result = proc_wait4(-1, &status, 0);
        if (result != child.pid) {
            fprintf(stderr, "wait4 returned %d, expected child %d\n",
                    result, child.pid);
            return 2;
        }
        if (!injected || parked_without_wake) {
            fprintf(stderr, "waiter crossed race window without safe wake/recheck\n");
            return 3;
        }
        puts("wait-registration-window: PASS");
    }

    if (parked_without_wake) {
        fprintf(stderr, "wait-registration-window: FAIL parked after child exit was lost\n");
        return 1;
    }

    /* A zombie thread-group leader remains a waitable child even while its
     * last member thread is still alive.  WNOHANG reports no status (0), not
     * ECHILD, then the same proc_wait4 body reaps it after group death. */
    waiter = (task_t){.pid=20, .tgid=20, .pgid=20};
    child = (task_t){.pid=21, .ppid=20, .tgid=21, .pgid=20,
                     .state=PROC_ZOMBIE, .parent=&waiter};
    live_member = (task_t){.pid=22, .ppid=20, .tgid=21, .pgid=20,
                           .state=PROC_RUNNABLE, .parent=&waiter};
    waiter.tg_leader = &waiter;
    waiter.children = &child;
    child.tg_leader = &child;
    child.tg_next = &live_member;
    live_group_test = 1;
    int status = 0;
    int result = proc_wait4(child.pid, &status, WNOHANG);
    if (result != 0) {
        fprintf(stderr, "live zombie leader wait4 returned %d, expected 0\n",
                result);
        return 5;
    }
    live_member.state = PROC_ZOMBIE;
    result = proc_wait4(child.pid, &status, WNOHANG);
    if (result != child.pid) {
        fprintf(stderr, "dead-group wait4 returned %d, expected %d\n",
                result, child.pid);
        return 6;
    }
    puts("wait-zombie-leader: PASS");
    return 0;
}
"""


def main() -> None:
    source = WAIT_SOURCE.read_text()
    helpers = [
        extract_function(source, "static void wait_accumulate_child_time("),
        extract_function(source, "static int wait_task_tgid("),
        extract_function(source, "static int wait_is_direct_child("),
        extract_function(source, "static int wait_is_child_for_waiter_locked("),
        extract_function(source, "static task_t *wait_group_start_locked("),
        extract_function(source, "static int wait_child_matches_locked("),
    ]
    wait4 = extract_function(source, "int proc_wait4(")
    with tempfile.TemporaryDirectory(prefix="a20-wait-window-") as tmp:
        tmp = Path(tmp)
        harness = tmp / "wait_window.c"
        binary = tmp / "wait_window"
        harness.write_text(PRELUDE + "\n".join(helpers) + "\n" + wait4 + "\n" + HARNESS)
        subprocess.run(["cc", "-std=gnu11", "-O0", "-Wall", "-Wextra",
                        "-Werror", str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()

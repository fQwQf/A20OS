#!/usr/bin/env python3
"""Exercise wait4 registration and the real child-wait notifier race windows."""
from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
WAIT_SOURCE = ROOT / "kernel/proc/wait.c"
EXIT_SOURCE = ROOT / "kernel/proc/exit.c"


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


NOTIFIER_PRELUDE = r"""
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#define PROC_WAKE_EVENT 3
typedef struct spinlock {
    pthread_mutex_t mutex;
    int is_tasklist;
} spinlock_t;
typedef struct task task_t;
struct task {
    int pid, tgid, waiting_for_child;
    unsigned long wait_seq;
    spinlock_t park_lock;
};
typedef void (*notifier_fn_t)(task_t *);

static spinlock_t tasklist_lock = {
    .mutex = PTHREAD_MUTEX_INITIALIZER, .is_tasklist = 1
};
static task_t waiter, parent;
static pthread_mutex_t gate_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t gate_changed = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t wake_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t wake_changed = PTHREAD_COND_INITIALIZER;
static int scan_complete, exit_attempted_tasklist, exit_returned, wake_queued;
static _Thread_local int in_exit_thread;
static notifier_fn_t active_notifier;
static unsigned long g_proc_waiting_child_waiter_count;

static uint64_t spin_lock_irqsave(spinlock_t *lock) {
    if (lock == &tasklist_lock && in_exit_thread) {
        pthread_mutex_lock(&gate_lock);
        exit_attempted_tasklist = 1;
        pthread_cond_broadcast(&gate_changed);
        pthread_mutex_unlock(&gate_lock);
    }
    pthread_mutex_lock(&lock->mutex);
    return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    pthread_mutex_unlock(&lock->mutex);
}
static int proc_task_tgid(task_t *task) {
    return task->tgid > 0 ? task->tgid : task->pid;
}
static task_t *proc_first_task_locked(void) { return &waiter; }
static task_t *proc_next_task_locked(task_t *task) {
    (void)task;
    return NULL;
}
static int proc_try_wake_locked(task_t *task, unsigned long seq, int why) {
    (void)seq;
    (void)why;
    if (task != &waiter)
        return 0;
    pthread_mutex_lock(&wake_lock);
    wake_queued = 1;
    pthread_cond_broadcast(&wake_changed);
    pthread_mutex_unlock(&wake_lock);
    return 1;
}

static void *waiter_thread(void *unused) {
    (void)unused;
    /* Model proc_wait4 after its child scan while it still owns tasklist_lock. */
    pthread_mutex_lock(&tasklist_lock.mutex);
    pthread_mutex_lock(&gate_lock);
    scan_complete = 1;
    pthread_cond_broadcast(&gate_changed);
    while (!exit_attempted_tasklist && !exit_returned)
        pthread_cond_wait(&gate_changed, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    pthread_mutex_lock(&waiter.park_lock.mutex);
    waiter.waiting_for_child = 1;
    pthread_mutex_unlock(&waiter.park_lock.mutex);
    pthread_mutex_unlock(&tasklist_lock.mutex);

    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_nsec += 300000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }
    pthread_mutex_lock(&wake_lock);
    while (!wake_queued) {
        int rc = pthread_cond_timedwait(&wake_changed, &wake_lock, &deadline);
        if (rc != 0)
            break;
    }
    int woke = wake_queued;
    pthread_mutex_unlock(&wake_lock);
    return (void *)(uintptr_t)woke;
}

static void *exit_thread(void *unused) {
    (void)unused;
    pthread_mutex_lock(&gate_lock);
    while (!scan_complete)
        pthread_cond_wait(&gate_changed, &gate_lock);
    pthread_mutex_unlock(&gate_lock);

    in_exit_thread = 1;
    active_notifier(&parent);
    in_exit_thread = 0;

    pthread_mutex_lock(&gate_lock);
    exit_returned = 1;
    pthread_cond_broadcast(&gate_changed);
    pthread_mutex_unlock(&gate_lock);
    return NULL;
}

static int run_race(notifier_fn_t notifier) {
    pthread_t waiter_tid, exit_tid;
    void *waiter_result = NULL;
    memset(&waiter, 0, sizeof(waiter));
    memset(&parent, 0, sizeof(parent));
    pthread_mutex_init(&waiter.park_lock.mutex, NULL);
    pthread_mutex_init(&parent.park_lock.mutex, NULL);
    waiter.pid = 10;
    waiter.tgid = 10;
    waiter.wait_seq = 7;
    parent.pid = 20;
    parent.tgid = 10;
    scan_complete = exit_attempted_tasklist = exit_returned = wake_queued = 0;
    active_notifier = notifier;
    pthread_create(&waiter_tid, NULL, waiter_thread, NULL);
    pthread_create(&exit_tid, NULL, exit_thread, NULL);
    pthread_join(waiter_tid, &waiter_result);
    pthread_join(exit_tid, NULL);
    pthread_mutex_destroy(&waiter.park_lock.mutex);
    pthread_mutex_destroy(&parent.park_lock.mutex);
    return (int)(uintptr_t)waiter_result;
}

"""


NOTIFIER_HARNESS = r"""
int main(void) {
    if (!run_race(proc_wake_child_waiters)) {
        fprintf(stderr, "wait-notifier-registration-window: FAIL lost child wake\n");
        return 1;
    }
    puts("wait-notifier-registration-window: PASS");
    if (run_race(legacy_proc_wake_child_waiters)) {
        fprintf(stderr, "wait-notifier-zero-hint-control: FAIL expected lost wake\n");
        return 2;
    }
    puts("wait-notifier-zero-hint-control: PASS (lost wake reproduced)");
    return 0;
}
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
    exit_source = EXIT_SOURCE.read_text()
    helpers = [
        extract_function(source, "static void wait_accumulate_child_time("),
        extract_function(source, "static int wait_task_tgid("),
        extract_function(source, "static int wait_is_direct_child("),
        extract_function(source, "static int wait_is_child_for_waiter_locked("),
        extract_function(source, "static task_t *wait_group_start_locked("),
        extract_function(source, "static int wait_child_matches_locked("),
    ]
    wait4 = extract_function(source, "int proc_wait4(")
    notifier = extract_function(exit_source, "void proc_wake_child_waiters(")
    legacy_notifier = notifier.replace(
        "void proc_wake_child_waiters(",
        "static void legacy_proc_wake_child_waiters(",
        1,
    ).replace(
        "    if (!parent)\n        return;",
        "    if (!parent)\n        return;\n"
        "    if (!__atomic_load_n(&g_proc_waiting_child_waiter_count, "
        "__ATOMIC_RELAXED))\n        return;",
        1,
    )
    if "legacy_proc_wake_child_waiters" not in legacy_notifier or \
       "g_proc_waiting_child_waiter_count" not in legacy_notifier:
        raise ValueError("could not derive the zero-hint lost-wakeup control")
    notifier_harness = (
        NOTIFIER_PRELUDE + "\n" + notifier + "\n" + legacy_notifier + "\n"
        + NOTIFIER_HARNESS
    )
    with tempfile.TemporaryDirectory(prefix="a20-wait-window-") as tmp:
        tmp = Path(tmp)
        harness = tmp / "wait_window.c"
        binary = tmp / "wait_window"
        harness.write_text(PRELUDE + "\n".join(helpers) + "\n" + wait4 + "\n" + HARNESS)
        subprocess.run(["cc", "-std=gnu11", "-O0", "-Wall", "-Wextra",
                        "-Werror", str(harness), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)

        notifier_source = tmp / "wait_notifier_window.c"
        notifier_binary = tmp / "wait_notifier_window"
        notifier_source.write_text(notifier_harness)
        subprocess.run(["cc", "-std=gnu11", "-O0", "-Wall", "-Wextra",
                        "-Werror", "-pthread", str(notifier_source),
                        "-o", str(notifier_binary)], check=True)
        subprocess.run([str(notifier_binary)], check=True)


if __name__ == "__main__":
    main()

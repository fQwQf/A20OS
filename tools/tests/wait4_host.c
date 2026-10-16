/*
 * wait4 host differential harness (WAIT4_IMPL).
 *
 * Compiled twice by tools/tests/test_wait4_host.py from this same source:
 *
 *   - variant "c":    engine include = the retained C reference engine
 *                     extracted from kernel/proc/wait.c (WAIT4_COMMON +
 *                     WAIT4_C_ENGINE regions);
 *   - variant "rust": engine include = the WAIT4_RUST adapter extracted from
 *                     the same file, linked against the production Rust
 *                     controller object built from kernel/proc/wait4_state.rs.
 *
 * Both variants run the generated scenario table (WAIT4_SCENARIO_INC) against
 * the same deterministic world model.  The stub layer below is the only
 * kernel surface the engines can touch: locks, chain/field sampling, pin/
 * detach/destroy/put, group/current predicates, the park protocol and yield.
 * Every stub call is logged in order, and the harness enforces the lock rules
 * (tasklist outer, park_lock inner, no nested park locks, destroy/put after
 * the scan lock).  The driver requires byte-identical stdout from both
 * variants and checks the expected results per run.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "proc/wait4_state.h"

/* wait(2) option bits and sampled states mirrored from the kernel headers. */
#define WNOHANG     1
#define WUNTRACED   2
#define WCONTINUED  8
#define WNOWAIT     0x1000000
#define __WNOTHREAD 0x20000000

#define W_PROC_UNUSED  0
#define W_PROC_READY   1
#define W_PROC_RUNNING 2
#define W_PROC_BLOCKED 3
#define W_PROC_STOPPED 4
#define W_PROC_ZOMBIE  5

#define W_ERESTARTSYS 512
#define W_ECHILD      10
#define W_SIGTRAP     5

/* proc_wake_reason_t values used by proc_wake_reason_is_task_interrupt(). */
#define W_WAKE_EVENT       1
#define W_WAKE_SIGNAL      4
#define W_WAKE_FATAL       5
#define W_WAKE_TASK_EXIT   6

/* Kernel-side spellings used by the extracted production text. */
#define PROC_UNUSED            W_PROC_UNUSED
#define PROC_READY             W_PROC_READY
#define PROC_RUNNING           W_PROC_RUNNING
#define PROC_BLOCKED           W_PROC_BLOCKED
#define PROC_STOPPED           W_PROC_STOPPED
#define PROC_ZOMBIE            W_PROC_ZOMBIE
#define ECHILD                 W_ECHILD
#define ERESTARTSYS            W_ERESTARTSYS
#define SIGTRAP                W_SIGTRAP
#define PROC_WAKE_EVENT        W_WAKE_EVENT
#define PROC_WAKE_SIGNAL       W_WAKE_SIGNAL
#define PROC_WAKE_FATAL_SIGNAL W_WAKE_FATAL
#define PROC_WAKE_TASK_EXIT    W_WAKE_TASK_EXIT

#define W_MAX_TASKS  8
#define W_MAX_EVENTS 512

#define W_UNUSED __attribute__((unused))

typedef struct task_t task_t;

typedef struct w_spinlock {
    const char *name;
    task_t *owner;
} w_spinlock_t;

struct task_t {
    int pid, tgid, ppid, pgid;
    int state, exit_code;
    int stop_report_pending, continue_report_pending;
    int ptrace_stop_active, ptrace_event;
    int waiting_for_child;
    uint64_t total_time, stime_ticks, child_utime, child_stime;
    task_t *parent, *tg_leader, *tg_next, *children, *sibling_next;
    w_spinlock_t park_lock;
    int get_count, put_count, destroyed;
};

typedef struct w_wait_token {
    task_t *task;
    uint64_t seq;
    int prepare_error;
} proc_wait_token_t;

typedef int proc_wake_reason_t;
typedef int proc_wait_mode_t;
#define PROC_WAIT_INTERRUPTIBLE 1

static w_spinlock_t tasklist_lock = { .name = "tasklist", .owner = NULL };

typedef struct w_world {
    task_t tasks[W_MAX_TASKS];
    int task_count;
    task_t *self;
    int group_dead;
    long current_hits;
    long get_fail_left;
    int signal_pending;
    int park_reason;
    uint64_t park_seq;
    int lock_depth;
    int park_depth;
    int wake_count, yield_count, commit_count;
    char events[W_MAX_EVENTS][48];
    int event_count;
} w_world_t;

static w_world_t w;
static void (*g_on_commit)(void);
static int g_hook_slot;

static void ev(const char *fmt, ...)
{
    if (w.event_count >= W_MAX_EVENTS) {
        fprintf(stderr, "FAIL event overflow\n");
        exit(4);
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(w.events[w.event_count], sizeof(w.events[0]), fmt, ap);
    va_end(ap);
    w.event_count++;
}

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(4); \
    } \
} while (0)

/* ---- stub kernel surface used by both engines ---- */

static W_UNUSED uint64_t spin_lock_irqsave(w_spinlock_t *lock)
{
    if (lock == &tasklist_lock) {
        CHECK(w.lock_depth == 0);
        w.lock_depth++;
        ev("tl_lock");
    } else {
        CHECK(w.park_depth == 0);
        CHECK(w.lock_depth == 0 || w.lock_depth == 1);
        CHECK(lock->owner != NULL);
        w.park_depth++;
        ev("pl_lock:%d", lock->owner->pid);
    }
    return 0;
}

static W_UNUSED void spin_unlock_irqrestore(w_spinlock_t *lock, uint64_t flags)
{
    (void)flags;
    if (lock == &tasklist_lock) {
        CHECK(w.lock_depth == 1);
        w.lock_depth--;
        ev("tl_unlock");
    } else {
        CHECK(w.park_depth == 1);
        w.park_depth--;
        ev("pl_unlock:%d", lock->owner->pid);
    }
}

static W_UNUSED task_t *proc_current(void)
{
    ev("current");
    return w.self;
}

static W_UNUSED int proc_task_state_get(task_t *t)
{
    int state = W_PROC_UNUSED;
    if (t) {
        uint64_t f = spin_lock_irqsave(&t->park_lock);
        state = t->state;
        spin_unlock_irqrestore(&t->park_lock, f);
    }
    ev("state:%d=%d", t ? t->pid : -1, state);
    return state;
}

static W_UNUSED task_t *proc_get(task_t *t)
{
    CHECK(w.lock_depth == 1);
    CHECK(w.park_depth == 1);
    if (w.get_fail_left > 0) {
        w.get_fail_left--;
        ev("get_fail:%d", t->pid);
        return NULL;
    }
    ev("get:%d", t->pid);
    t->get_count++;
    return t;
}

static W_UNUSED void proc_reap_detach_list_locked(task_t *t)
{
    CHECK(w.lock_depth == 1);
    CHECK(w.park_depth == 0);
    ev("detach:%d", t->pid);
    task_t *p = t->parent;
    if (p) {
        task_t **pp = &p->children;
        while (*pp && *pp != t)
            pp = &(*pp)->sibling_next;
        if (*pp == t)
            *pp = t->sibling_next;
    }
    t->sibling_next = NULL;
}

static W_UNUSED void proc_destroy_task(task_t *t)
{
    CHECK(w.lock_depth == 0);
    CHECK(w.park_depth == 0);
    ev("destroy:%d", t->pid);
    t->destroyed = 1;
}

static W_UNUSED void proc_put(task_t *t)
{
    if (!t)
        return;
    CHECK(w.lock_depth == 0);
    CHECK(w.park_depth == 0);
    ev("put:%d", t->pid);
    t->put_count++;
}

static W_UNUSED int proc_tg_group_dead_locked(task_t *t)
{
    CHECK(w.lock_depth == 1);
    ev("gdead:%d=%d", t->pid, w.group_dead);
    return w.group_dead;
}

static W_UNUSED int proc_task_is_current_any_cpu(task_t *t)
{
    int v = w.current_hits > 0;
    if (v != 0)
        w.current_hits--;
    ev("current_cpu:%d=%d", t->pid, v);
    return v;
}

static W_UNUSED void proc_yield(void)
{
    CHECK(w.lock_depth == 0);
    ev("yield");
    w.yield_count++;
    CHECK(w.yield_count <= 16);
}

static W_UNUSED proc_wait_token_t proc_park_prepare_locked(proc_wait_mode_t mode,
                                                           uint64_t deadline)
{
    (void)mode;
    (void)deadline;
    CHECK(w.park_depth == 1);
    w.park_seq++;
    ev("prepare:%llu", (unsigned long long)w.park_seq);
    return (proc_wait_token_t){ .task = w.self, .seq = w.park_seq,
                                .prepare_error = 0 };
}

static W_UNUSED int signal_task_has_unblocked(task_t *t)
{
    CHECK(w.park_depth == 1);
    ev("sigchk:%d=%d", t->pid, w.signal_pending);
    return w.signal_pending;
}

static W_UNUSED int proc_try_wake_locked(task_t *t, uint64_t seq,
                                         proc_wake_reason_t reason)
{
    (void)reason;
    CHECK(w.park_depth == 1);
    ev("wake:%d:%llu", t->pid, (unsigned long long)seq);
    w.wake_count++;
    return 1;
}

static W_UNUSED proc_wake_reason_t proc_park_commit(proc_wait_token_t token)
{
    (void)token;
    CHECK(w.lock_depth == 0);
    CHECK(w.park_depth == 0);
    w.commit_count++;
    CHECK(w.commit_count <= 16);
    ev("commit:%d", w.park_reason);
    if (g_on_commit)
        g_on_commit();
    return w.park_reason;
}

static W_UNUSED void proc_park_finish(proc_wait_token_t token)
{
    (void)token;
    ev("finish");
}

static W_UNUSED int proc_wake_reason_is_task_interrupt(proc_wake_reason_t reason)
{
    return reason == W_WAKE_SIGNAL || reason == W_WAKE_FATAL ||
           reason == W_WAKE_TASK_EXIT;
}

/* ---- world construction helpers used by the generated scenarios ---- */

static W_UNUSED task_t *wt(w_world_t *world, int index)
{
    CHECK(index >= 0 && index < W_MAX_TASKS);
    if (index + 1 > world->task_count)
        world->task_count = index + 1;
    return &world->tasks[index];
}

static W_UNUSED void wcfg(task_t *t, int pid, int tgid, int ppid, int pgid,
                          int state)
{
    memset(t, 0, sizeof(*t));
    t->pid = pid;
    t->tgid = tgid;
    t->ppid = ppid;
    t->pgid = pgid;
    t->state = state;
    t->park_lock = (w_spinlock_t){ .name = "park", .owner = t };
    t->tg_leader = t;
}

static W_UNUSED void wlink(task_t *parent, task_t *child)
{
    child->parent = parent;
    child->ppid = parent->pid;
    child->sibling_next = parent->children;
    parent->children = child;
}

static W_UNUSED void wmember(task_t *leader, task_t *member)
{
    member->tg_leader = leader;
    member->tg_next = leader->tg_next;
    leader->tg_next = member;
}

/* ---- named pre-run hooks for the generated scenarios ---- */

static W_UNUSED void pre_group_dead(void)      { w.group_dead = 1; }
static W_UNUSED void pre_get_fail(void)        { w.get_fail_left = 1; }
static W_UNUSED void pre_current_hits(void)    { w.current_hits = 1; }
static W_UNUSED void pre_signal_pending(void)  { w.signal_pending = 1; }
static W_UNUSED void pre_task_exit_wake(void)  { w.park_reason = W_WAKE_TASK_EXIT; }
static W_UNUSED void pre_park_event(void)      { w.park_reason = W_WAKE_EVENT; }

static W_UNUSED void hook_exit_slot(void)
{
    task_t *t = &w.tasks[g_hook_slot];
    t->state = W_PROC_ZOMBIE;
    t->exit_code = 4;
    w.group_dead = 1;
}

static W_UNUSED void pre_event_exit_child(void)
{
    w.park_reason = W_WAKE_EVENT;
    g_on_commit = hook_exit_slot;
}

/* ---- scenario table driver ---- */

typedef struct wrun {
    int pid;
    int options;
    int target;      /* task slot index or -1 */
    int use_status;  /* 1: pass a status slot, 0: pass NULL */
    void (*pre)(void);
} wrun_t;

typedef struct wscenario {
    const char *name;
    void (*setup)(w_world_t *world);
    int run_count;
    wrun_t runs[6];
} wscenario_t;

#include WAIT4_ENGINE_INC

#include WAIT4_SCENARIO_INC

static void print_digest(void)
{
    printf("  world lock=%d park=%d wake=%d yield=%d commits=%d\n",
           w.lock_depth, w.park_depth, w.wake_count, w.yield_count,
           w.commit_count);
    for (int i = 0; i < w.task_count; i++) {
        task_t *t = &w.tasks[i];
        int in_list = 0;
        for (task_t *c = t->parent ? t->parent->children : NULL; c;
             c = c->sibling_next)
            if (c == t)
                in_list = 1;
        printf("  T%d pid=%d state=%d exit=%d sp=%d cp=%d pt=%d/%d waited=%d "
               "gets=%d puts=%d destroyed=%d in_list=%d cut=%llu cst=%llu\n",
               i, t->pid, t->state, t->exit_code, t->stop_report_pending,
               t->continue_report_pending, t->ptrace_stop_active,
               t->ptrace_event, t->waiting_for_child, t->get_count,
               t->put_count, t->destroyed, in_list,
               (unsigned long long)t->child_utime,
               (unsigned long long)t->child_stime);
    }
}

static void run_one(const char *scenario, const wrun_t *r, int run_index)
{
    int status = -777;
    int *status_ptr = r->use_status ? &status : NULL;
    task_t *target = r->target >= 0 ? &w.tasks[r->target] : NULL;
    int before_events = w.event_count;

    int ret = proc_wait4_internal(r->pid, target, status_ptr, r->options);

    printf("RUN %s[%d] rc=%d status=%d\n", scenario, run_index, ret, status);
    for (int i = before_events; i < w.event_count; i++)
        printf("  ev %s\n", w.events[i]);
    print_digest();
}

int main(void)
{
    fprintf(stderr, "variant=%s scenarios=%d\n", WAIT4_VARIANT,
            g_scenario_count);
    for (int s = 0; s < g_scenario_count; s++) {
        const wscenario_t *sc = &g_scenarios[s];
        memset(&w, 0, sizeof(w));
        g_on_commit = NULL;
        g_hook_slot = 0;
        w.park_reason = W_WAKE_EVENT;
        w.group_dead = 1;
        w.self = &w.tasks[0];
        if (sc->setup)
            sc->setup(&w);
        for (int r = 0; r < sc->run_count; r++) {
            if (sc->runs[r].pre)
                sc->runs[r].pre();
            run_one(sc->name, &sc->runs[r], r);
        }
    }
    printf("WAIT4_HOST: DONE\n");
    return 0;
}

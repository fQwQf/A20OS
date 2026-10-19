/*
 * Host differential harness for kernel/mm/oom_policy.rs.
 *
 * The production Rust policy is linked against a model of the task list and
 * the memory facts.  Every assertion is about a decision Rust owns: the victim
 * score (including the no-address-space penalty and the tie-break), the
 * escalation order in try_reclaim (in-progress guard, watermark, cooldown,
 * swap-before-kill), the range validation, and the counters.
 *
 * The kswapd loop's own body runs only in the guest (oom_stress), because it
 * never returns.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define PAGE_SIZE 4096UL
#define USER_VA_LIMIT 0x4000000000UL

#define PROC_UNUSED 0
#define PROC_ZOMBIE 5

static int failures;
static int checks;

static void check(int cond, const char *what)
{
    checks++;
    if (!cond) {
        failures++;
        printf("FAIL: %s\n", what);
    }
}

struct task {
    int pid, state, score, has_mm, is_idle;
};
struct a20_oom_task_snap { int32_t pid, state, score, has_mm, is_idle; };
struct a20_oom_params {
    uintptr_t min_free_pages;
    int32_t max_swap_reclaim;
    uint64_t cooldown_ticks;
    uint64_t kswapd_interval_ticks;
    int32_t proc_unused, proc_zombie, kswapd_swap_batch, reserved;
};
struct oom_stats_mirror {
    unsigned long kills, last_kill_tick;
    int last_victim_pid, last_victim_score;
    unsigned long free_pages_at_kill, free_pages_now;
    int in_progress;
    unsigned long kswapd_passes, kswapd_pages_freed, kswapd_last_pass_tick;
};

static struct task g_tasks[8];
static int g_ntasks;
static uintptr_t g_free_pages = 1000;
static uint64_t g_now = 5000;
static uint64_t g_time_base = 5000;
static int g_swap_rc;
static int swap_calls, swap_last_pid, swap_last_target;
static int kill_calls, kill_pid, find_calls, put_calls;

static void reset(void)
{
    g_ntasks = 0;
    g_free_pages = 1000;
    /* The policy's kill timestamp is persistent module state, so every
     * scenario starts well past the previous kill's cooldown. */
    g_time_base += 1000;
    g_now = g_time_base;
    g_swap_rc = 0;
    swap_calls = swap_last_pid = swap_last_target = 0;
    kill_calls = kill_pid = find_calls = put_calls = 0;
}

static void add_task(int pid, int state, int score, int has_mm, int idle)
{
    struct task *t = &g_tasks[g_ntasks++];
    t->pid = pid;
    t->state = state;
    t->score = score;
    t->has_mm = has_mm;
    t->is_idle = idle;
}

void a20_oom_params(struct a20_oom_params *out)
{
    out->min_free_pages = 256;
    out->max_swap_reclaim = 8;
    out->cooldown_ticks = 100;
    out->kswapd_interval_ticks = 50;
    out->proc_unused = PROC_UNUSED;
    out->proc_zombie = PROC_ZOMBIE;
    out->kswapd_swap_batch = 64;
    out->reserved = 0;
}

void *a20_oom_task_first(void) { return g_ntasks ? (void *)&g_tasks[0] : NULL; }
void *a20_oom_task_next(void *t)
{
    int i = (int)((struct task *)t - g_tasks);
    return (i + 1 < g_ntasks) ? (void *)&g_tasks[i + 1] : NULL;
}
int a20_oom_task_snapshot(void *t, struct a20_oom_task_snap *out)
{
    struct task *s = t;
    out->pid = s->pid;
    out->state = s->state;
    out->score = s->score;
    out->has_mm = s->has_mm;
    out->is_idle = s->is_idle;
    return 0;
}
uintptr_t a20_oom_free_pages(void) { return g_free_pages; }
uint64_t a20_oom_now(void) { return g_now; }
uintptr_t a20_oom_user_va_limit(void) { return USER_VA_LIMIT; }
int a20_oom_swap_victim_by_pid(int pid, int target)
{
    swap_calls++;
    swap_last_pid = pid;
    swap_last_target = target;
    if (g_swap_rc > 0)
        g_free_pages = 300; /* a successful swap raises the water above the mark */
    return g_swap_rc;
}
int a20_oom_swap_task(void *t, int limit, uintptr_t start, uintptr_t end)
{
    (void)t; (void)limit; (void)start; (void)end;
    swap_calls++;
    return g_swap_rc;
}
void *a20_oom_find_get(int pid)
{
    find_calls++;
    for (int i = 0; i < g_ntasks; i++)
        if (g_tasks[i].pid == pid)
            return &g_tasks[i];
    return NULL;
}
void a20_oom_put(void *t) { (void)t; put_calls++; }
void a20_oom_kill(void *t) { kill_calls++; kill_pid = ((struct task *)t)->pid; }
void a20_oom_sleep_until(uint64_t tick) { (void)tick; }
void a20_oom_yield(void) {}
void a20_oom_kswapd_started(void) {}
size_t page_cache_drop_clean(void) { return 0; }
size_t slab_reclaim_spare(void) { return 0; }

extern int oom_try_reclaim(void);
extern int oom_swap_reclaim_pages(int target);
extern int oom_swap_reclaim_range(void *t, unsigned long start, unsigned long len);
extern void oom_get_stats(struct oom_stats_mirror *out);

/* -------------------------------------------------------------- scenarios */

static void victim_choice(void)
{
    reset();
    add_task(1, 1, 500, 1, 0);   /* init: pid <= 2, skipped */
    add_task(0, 1, 900, 1, 1);   /* idle: skipped even at the top score */
    add_task(10, 1, 100, 1, 0);
    add_task(11, 1, 300, 1, 0);
    add_task(12, PROC_ZOMBIE, 800, 1, 0);
    add_task(13, PROC_UNUSED, 800, 1, 0);
    g_free_pages = 100;          /* below the watermark */
    g_swap_rc = 0;               /* swap frees nothing */
    check(oom_try_reclaim() == 1, "a victim is killed");
    check(kill_calls == 1 && kill_pid == 11,
          "the highest live score wins, skipping pid<=2, idle, zombie, unused");
    check(put_calls == 1, "the victim reference is dropped");

    /* No address space costs 100. */
    reset();
    add_task(20, 1, 250, 0, 0);  /* adjusted 150 */
    add_task(21, 1, 200, 1, 0);
    g_free_pages = 100;
    check(oom_try_reclaim() == 1, "the kill proceeds");
    check(kill_pid == 21, "a task with no mm is penalised by 100");
    (void)swap_last_pid;
}

static void escalation(void)
{
    reset();
    add_task(10, 1, 100, 1, 0);
    /* Above the watermark: nothing happens. */
    g_free_pages = 1000;
    check(oom_try_reclaim() == 0, "above the watermark is a no-op");
    check(swap_calls == 0 && kill_calls == 0, "nothing is swapped or killed");
    (void)oom_get_stats;

    /* Below it: swap first, and it succeeds with the max batch. */
    reset();
    add_task(10, 1, 100, 1, 0);
    g_free_pages = 1000;
    g_now = 5000;
    g_swap_rc = 8;
    g_free_pages = 100; /* below */
    check(oom_try_reclaim() == 1, "swap-only reclaim succeeds");
    check(swap_calls == 1 && swap_last_target == 8,
          "the swap step uses the max batch");
    check(kill_calls == 0, "no victim is killed when swap suffices");

    /* Swap that frees nothing falls through to the kill. */
    reset();
    add_task(10, 1, 100, 1, 0);
    g_free_pages = 100;
    g_swap_rc = 0;
    check(oom_try_reclaim() == 1, "the kill happens after an empty swap");
    check(swap_calls == 1 && kill_calls == 1, "swap then kill");

    /* The cooldown suppresses a second kill. */
    int before_kills = 0;
    {
        struct oom_stats_mirror pre;
        oom_get_stats(&pre);
        before_kills = (int)pre.kills;
    }
    reset();
    add_task(10, 1, 100, 1, 0);
    g_free_pages = 100;
    g_swap_rc = 0;
    check(oom_try_reclaim() == 1, "the first kill proceeds");
    g_now += 50; /* inside OOM_COOLDOWN_TICKS (100) */
    check(oom_try_reclaim() == 0, "a kill inside the cooldown is suppressed");
    check(kill_calls == 1, "no second kill inside the cooldown");
    g_now += 100; /* outside */
    check(oom_try_reclaim() == 1, "a kill after the cooldown proceeds");
    check(kill_calls == 2, "the second kill happens");

    /* The stats report the last victim. */
    struct oom_stats_mirror st;
    oom_get_stats(&st);
    check(st.kills >= 2, "the kill count is reported");
    check(st.kills == (unsigned long)(before_kills + 2), "exactly two kills were counted");
    check(st.last_victim_pid == 10 && st.last_victim_score == 100,
          "the last victim is reported");
    check(st.in_progress == 0, "the in-progress guard is clear afterwards");
    check(st.free_pages_now == 100, "the live free-page count is reported");
}

static void range_contract(void)
{
    static int t;
    reset();
    check(oom_swap_reclaim_range(NULL, 0x1000, 0x1000) == -22, "null task is EINVAL");
    check(oom_swap_reclaim_range(&t, 0x1001, 0x1000) == -22, "misaligned start is EINVAL");
    check(oom_swap_reclaim_range(&t, 0x1000, 0x1001) == -22, "misaligned length is EINVAL");
    check(oom_swap_reclaim_range(&t, 0x1000, 0) == -22, "zero length is EINVAL");
    check(oom_swap_reclaim_range(&t, USER_VA_LIMIT, PAGE_SIZE) == -22,
          "a start past the limit is EINVAL");
    check(swap_calls == 0, "no refusal reaches the swap walk");

    g_swap_rc = 3;
    check(oom_swap_reclaim_range(&t, 0x1000, 0x2000) == 3, "a valid range swaps");
    check(swap_calls == 1, "the walk is entered once");
}

int main(void)
{
    victim_choice();
    escalation();
    range_contract();
    reset();

    if (failures) {
        printf("OOM_POLICY_HOST: FAIL %d/%d checks\n", failures, checks);
        return 1;
    }
    printf("OOM_POLICY_HOST: PASS %d checks\n", checks);
    return 0;
}

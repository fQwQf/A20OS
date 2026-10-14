#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

typedef struct fake_task {
    int refs;
    int index;
    uint64_t deadline;
    uint64_t seq;
    uint64_t wake_time;
} fake_task_t;

struct wait_timer_stats {
    uint32_t entries;
    uint32_t capacity;
    uint64_t full_failures;
    uint64_t duplicate_rejections;
    uint64_t stale_expirations;
    uint32_t violations;
};

void a20_wait_timer_init_rust(void);
int a20_wait_timer_register_rust(fake_task_t *, uint64_t, uint64_t);
void a20_wait_timer_cancel_rust(fake_task_t *, uint64_t);
void a20_wait_timer_expire_rust(uint64_t);
void a20_wait_timer_stats_rust(struct wait_timer_stats *);

static int heap_locked;
static uint32_t heap_capacity = 1;
static uint64_t next_deadline = UINT64_MAX;
static uint64_t fired[8];
static unsigned fired_count;
static int fire_result;
static int cancel_after_collect;
static unsigned ref_observed_during_batch;

uint64_t a20_wait_timer_lock(void)
{
    int old = heap_locked;
    if (old) abort();
    heap_locked = 1;
    return (uint64_t)old;
}

void a20_wait_timer_unlock(uint64_t flags)
{
    if (!heap_locked) abort();
    heap_locked = (int)flags;
}

uint32_t a20_wait_timer_capacity(void) { return heap_capacity; }
fake_task_t *a20_wait_timer_task_ref(fake_task_t *task)
{
    task->refs++;
    return task;
}
void a20_wait_timer_task_put(fake_task_t *task)
{
    if (!task || task->refs <= 0) abort();
    task->refs--;
}
int a20_wait_timer_task_index(fake_task_t *task)
{
    return task ? task->index : -1;
}
void a20_wait_timer_set_task_index(fake_task_t *task, int index)
{
    if (task) task->index = index;
}
void a20_wait_timer_set_wake_time(fake_task_t *task, uint64_t deadline)
{
    if (task) task->wake_time = deadline;
}
void a20_wait_timer_publish_next(uint64_t deadline) { next_deadline = deadline; }
void a20_wait_timer_rearm_if_earlier(uint64_t deadline)
{
    if (deadline < next_deadline) next_deadline = deadline;
}
int a20_wait_timer_entry_valid(fake_task_t *task, unsigned index,
                               uint64_t deadline, uint64_t seq)
{
    return task && task->index == (int)index && task->deadline == deadline &&
           task->seq == seq;
}
int a20_wait_timer_fire_timeout(fake_task_t *task, uint64_t seq)
{
    (void)task;
    fired[fired_count++] = seq;
    return fire_result;
}
void a20_wait_timer_after_dequeue(fake_task_t *task, uint64_t seq)
{
    (void)seq;
    if (task->refs != 2) abort();
    ref_observed_during_batch++;
    if (cancel_after_collect)
        a20_wait_timer_cancel_rust(task, task->seq);
}
void proc_wait_timer_test_after_fire(fake_task_t *task, uint64_t seq,
                                     int result)
{
    (void)task;
    (void)seq;
    (void)result;
}

static void check(int condition, const char *message)
{
    if (!condition) {
        fprintf(stderr, "FAIL wait-timer host: %s\n", message);
        exit(1);
    }
}

int main(void)
{
    fake_task_t a = {.refs = 1, .index = -1};
    fake_task_t b = {.refs = 1, .index = -1};
    struct wait_timer_stats stats;

    a20_wait_timer_init_rust();
    a.deadline = 30;
    a.seq = 1;
    check(a20_wait_timer_register_rust(&a, 30, 1) == 0,
          "first registration succeeds");
    check(a.refs == 2 && a.index == 0 && a.wake_time == 30,
          "registration owns one task reference and publishes index/deadline");
    check(a20_wait_timer_register_rust(&b, 20, 2) == -2,
          "capacity failure returned");
    check(b.refs == 1 && b.index == -1,
          "capacity failure occurs before task reference acquisition");
    check(a20_wait_timer_register_rust(&a, 31, 3) == -3,
          "duplicate membership rejected");
    a20_wait_timer_stats_rust(&stats);
    check(stats.entries == 1 && stats.capacity == 1 &&
          stats.full_failures == 1 && stats.duplicate_rejections == 1 &&
          stats.violations == 0, "capacity/duplicate stats and heap audit");
    a20_wait_timer_cancel_rust(&a, 1);
    check(a.refs == 1 && a.index == -1 && a.wake_time == 0,
          "cancel clears metadata and releases timer reference");

    heap_capacity = 4;
    a20_wait_timer_init_rust();
    a.deadline = 30;
    a.seq = 4;
    b.deadline = 10;
    b.seq = 5;
    check(a20_wait_timer_register_rust(&a, 30, 4) == 0 &&
          a20_wait_timer_register_rust(&b, 10, 5) == 0,
          "multi-entry registration succeeds");
    check(next_deadline == 10, "minimum deadline is published");
    fire_result = 1;
    a20_wait_timer_expire_rust(10);
    check(fired_count == 1 && fired[0] == 5 && b.refs == 1 &&
          b.index == -1, "earliest entry expires and its reference is released");

    /* The dequeue callback is after heap-lock release. Simulate a competing
     * cancel there; the expiration batch must retain its ref through callback. */
    cancel_after_collect = 1;
    fire_result = 0;
    a20_wait_timer_expire_rust(30);
    cancel_after_collect = 0;
    check(fired_count == 2 && fired[1] == 4,
          "collected timer callback runs once after competing cancel");
    check(ref_observed_during_batch == 2 && a.refs == 1 && a.index == -1,
          "batch reference remains live until callback completes");
    a20_wait_timer_stats_rust(&stats);
    check(stats.entries == 0 && stats.violations == 0,
          "expiration leaves an empty valid heap");

    a.deadline = 40;
    a.seq = 6;
    check(a20_wait_timer_register_rust(&a, 40, 6) == 0,
          "stale-sequence fixture registration succeeds");
    fire_result = 2;
    a20_wait_timer_expire_rust(40);
    a20_wait_timer_stats_rust(&stats);
    check(stats.entries == 0 && stats.stale_expirations == 1 &&
          a.refs == 1 && a.index == -1,
          "stale expiry is counted and still releases its batch reference");
    puts("WAIT_TIMER_HOST: PASS capacity rollback duplicate stale cancel ordering batch-reference audit");
    return 0;
}

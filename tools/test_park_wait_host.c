#include "proc/park_wait_state.h"

#include <stddef.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef TEST_PARK_WAIT_RUST
#define PW_INIT a20_rust_park_wait_init
#define PW_PREPARE a20_rust_park_wait_prepare
#define PW_ROLLBACK a20_rust_park_wait_prepare_rollback
#define PW_WAKE a20_rust_park_wait_wake
#define PW_CANCEL a20_rust_park_wait_cancel
#define PW_COMMIT a20_rust_park_wait_commit
#define PW_FINISH a20_rust_park_wait_finish
#define PW_REASON a20_rust_park_wait_reason
#else
#define PW_INIT a20_c_park_wait_init
#define PW_PREPARE a20_c_park_wait_prepare
#define PW_ROLLBACK a20_c_park_wait_prepare_rollback
#define PW_WAKE a20_c_park_wait_wake
#define PW_CANCEL a20_c_park_wait_cancel
#define PW_COMMIT a20_c_park_wait_commit
#define PW_FINISH a20_c_park_wait_finish
#define PW_REASON a20_c_park_wait_reason
#endif

_Static_assert(sizeof(a20_park_wait_fields_t) == 5 * sizeof(void *),
               "C/Rust park-wait view size changed");
_Static_assert(offsetof(a20_park_wait_fields_t, wait_deadline) ==
                   sizeof(void *) &&
               offsetof(a20_park_wait_fields_t, park_state) ==
                   2 * sizeof(void *) &&
               offsetof(a20_park_wait_fields_t, wake_reason) ==
                   4 * sizeof(void *),
               "C/Rust park-wait view offsets changed");

typedef struct fixture {
    uint64_t seq;
    uint64_t deadline;
    int state;
    int mode;
    int reason;
    int timer_index; /* C timer heap owns this; it is deliberately not in view. */
    uint64_t wake_time;
    unsigned callbacks;
    pthread_mutex_t lock;
    a20_park_wait_fields_t view;
} fixture_t;

static unsigned checks;

#define CHECK(expr) do { \
    checks++; \
    if (!(expr)) { \
        fprintf(stderr, "FAIL line=%d: %s\n", __LINE__, #expr); \
        return 1; \
    } \
} while (0)

static void fixture_init(fixture_t *f)
{
    *f = (fixture_t){0};
    pthread_mutex_init(&f->lock, NULL);
    f->timer_index = -1;
    f->view = (a20_park_wait_fields_t){
        .wait_seq = &f->seq,
        .wait_deadline = &f->deadline,
        .park_state = &f->state,
        .wait_mode = &f->mode,
        .wake_reason = &f->reason,
    };
    PW_INIT(&f->view); /* unpublished-state initialization is lock-free */
}

static uint64_t lock_fixture(fixture_t *f)
{
    pthread_mutex_lock(&f->lock);
    return 0;
}

static void unlock_fixture(fixture_t *f, uint64_t flags)
{
    (void)flags;
    pthread_mutex_unlock(&f->lock);
}

static int prepare(fixture_t *f, int mode, uint64_t deadline, uint64_t *seq)
{
    uint64_t flags = lock_fixture(f);
    int ret = PW_PREPARE(&f->view, mode, deadline, seq);
    unlock_fixture(f, flags);
    return ret;
}

static void callback(void *context, uint64_t seq)
{
    fixture_t *f = context;
    if (!f || seq != f->seq)
        abort();
    /* The production callback runs under park_lock, before state publication. */
    int lock_result = pthread_mutex_trylock(&f->lock);
    if (lock_result == 0) {
        pthread_mutex_unlock(&f->lock);
        abort();
    }
    if (f->state != A20_PARK_PREPARING && f->state != A20_PARK_PARKED &&
        f->state != A20_PARK_WOKEN)
        abort();
    f->timer_index = -1;
    f->callbacks++;
}

static int finish(fixture_t *f, uint64_t seq, int timer_pending)
{
    uint64_t flags = 0;
    if (timer_pending)
        flags = lock_fixture(f);
    /* Match proc_park_finish's publication order: clear before release-IDLE. */
    __atomic_store_n(&f->wake_time, 0, __ATOMIC_RELAXED);
    int ret = PW_FINISH(&f->view, seq, timer_pending,
                        timer_pending ? callback : NULL,
                        timer_pending ? f : NULL);
    if (timer_pending)
        unlock_fixture(f, flags);
    return ret;
}

static int test_prepare_rollback_and_wrap(void)
{
    fixture_t f;
    fixture_init(&f);
    CHECK(f.state == A20_PARK_IDLE && f.seq == 0);
    uint64_t seq = 0;
    CHECK(prepare(&f, A20_WAIT_INTERRUPTIBLE, 100, &seq) == 0);
    CHECK(seq == 1 && f.state == A20_PARK_PREPARING && f.deadline == 100);
    CHECK(prepare(&f, A20_WAIT_KILLABLE, 0, &seq) == -1);
    uint64_t flags = lock_fixture(&f);
    PW_ROLLBACK(&f.view, 1); /* timer registration failure rollback */
    unlock_fixture(&f, flags);
    CHECK(f.state == A20_PARK_IDLE && f.deadline == 0 &&
          f.mode == A20_WAIT_UNINTERRUPTIBLE);

    __atomic_store_n(&f.seq, UINT64_MAX, __ATOMIC_RELAXED);
    CHECK(prepare(&f, A20_WAIT_KILLABLE, 0, &seq) == 0);
    CHECK(seq == 1); /* zero is skipped on sequence wrap */
    flags = lock_fixture(&f);
    PW_ROLLBACK(&f.view, seq);
    unlock_fixture(&f, flags);
    pthread_mutex_destroy(&f.lock);
    return 0;
}

static int test_reject_invalid_api_arguments(void)
{
    uint64_t seq = 1;
    int reason = -1;
    CHECK(PW_PREPARE(NULL, A20_WAIT_INTERRUPTIBLE, 1, &seq) == -1);
    CHECK(PW_PREPARE(NULL, A20_WAIT_INTERRUPTIBLE, 1, NULL) == -1);
    CHECK(PW_WAKE(NULL, 1, A20_WAKE_TIMEOUT, NULL, NULL) == 0);
    CHECK(PW_CANCEL(NULL, 1, NULL, NULL) == 0);
    CHECK(PW_COMMIT(NULL, 1, &reason) == -1);
    CHECK(PW_COMMIT(NULL, 1, NULL) == -1);
    CHECK(PW_FINISH(NULL, 1, 0, NULL, NULL) == 0);
    CHECK(PW_REASON(NULL, 1) == A20_WAKE_CANCEL);
    return 0;
}

static int test_wake_commit_timeout_exit_and_finish(void)
{
    fixture_t f;
    fixture_init(&f);
    uint64_t seq = 0;
    CHECK(prepare(&f, A20_WAIT_INTERRUPTIBLE, 200, &seq) == 0);
    f.timer_index = 4;
    uint64_t flags = lock_fixture(&f);
    CHECK(PW_WAKE(&f.view, seq, A20_WAKE_SIGNAL, callback, &f) == 1);
    unlock_fixture(&f, flags);
    CHECK(f.callbacks == 1 && f.timer_index == -1 &&
          f.state == A20_PARK_WOKEN);
    int reason = -1;
    flags = lock_fixture(&f);
    CHECK(PW_COMMIT(&f.view, seq, &reason) == 0);
    unlock_fixture(&f, flags);
    CHECK(reason == A20_WAKE_SIGNAL && PW_REASON(&f.view, seq) == reason);
    f.wake_time = 999;
    CHECK(finish(&f, seq, 0) == 1);
    CHECK(f.wake_time == 0 && f.state == A20_PARK_IDLE &&
          f.reason == A20_WAKE_NONE);

    CHECK(prepare(&f, A20_WAIT_KILLABLE, 300, &seq) == 0);
    flags = lock_fixture(&f);
    CHECK(PW_COMMIT(&f.view, seq, &reason) == 1);
    unlock_fixture(&f, flags);
    CHECK(f.state == A20_PARK_PARKED);
    f.timer_index = 7;
    flags = lock_fixture(&f);
    CHECK(PW_WAKE(&f.view, seq, A20_WAKE_TIMEOUT, callback, &f) == 2);
    unlock_fixture(&f, flags);
    CHECK(f.callbacks == 2 && f.state == A20_PARK_WOKEN);
    CHECK(PW_REASON(&f.view, seq) == A20_WAKE_TIMEOUT);
    f.wake_time = 1234;
    CHECK(finish(&f, seq, 0) == 1);
    CHECK(f.wake_time == 0 && f.state == A20_PARK_IDLE);

    CHECK(prepare(&f, A20_WAIT_KILLABLE, 400, &seq) == 0);
    flags = lock_fixture(&f);
    CHECK(PW_WAKE(&f.view, seq, A20_WAKE_TASK_EXIT, NULL, NULL) == 1);
    unlock_fixture(&f, flags);
    CHECK(PW_REASON(&f.view, seq) == A20_WAKE_TASK_EXIT);
    CHECK(finish(&f, seq, 0) == 1);
    pthread_mutex_destroy(&f.lock);
    return 0;
}

static int test_cancel_rules_and_stale_tokens(void)
{
    fixture_t f;
    fixture_init(&f);
    uint64_t seq = 0;
    CHECK(prepare(&f, A20_WAIT_UNINTERRUPTIBLE, 50, &seq) == 0);
    f.timer_index = 1;
    uint64_t flags = lock_fixture(&f);
    CHECK(PW_WAKE(&f.view, seq, A20_WAKE_SIGNAL, callback, &f) == 0);
    CHECK(PW_WAKE(&f.view, seq, A20_WAKE_TASK_EXIT, callback, &f) == 0);
    CHECK(PW_CANCEL(&f.view, seq, callback, &f) == 1);
    unlock_fixture(&f, flags);
    CHECK(f.callbacks == 1 && f.reason == A20_WAKE_CANCEL);
    CHECK(PW_REASON(&f.view, seq - 1) == A20_WAKE_CANCEL);
    CHECK(finish(&f, seq - 1, 0) == 0 && f.state == A20_PARK_WOKEN);
    CHECK(finish(&f, seq, 0) == 1 && f.state == A20_PARK_IDLE);

    CHECK(prepare(&f, A20_WAIT_KILLABLE, 0, &seq) == 0);
    flags = lock_fixture(&f);
    CHECK(PW_WAKE(&f.view, seq, A20_WAKE_SIGNAL, NULL, NULL) == 0);
    CHECK(PW_WAKE(&f.view, seq - 1, A20_WAKE_TIMEOUT, NULL, NULL) == 0);
    PW_ROLLBACK(&f.view, seq);
    unlock_fixture(&f, flags);
    CHECK(f.state == A20_PARK_IDLE && f.callbacks == 1);
    pthread_mutex_destroy(&f.lock);
    return 0;
}

typedef struct race_call {
    fixture_t *fixture;
    uint64_t seq;
    int op;
    int result;
    int reason;
} race_call_t;

static void *race_transition(void *arg)
{
    race_call_t *call = arg;
    fixture_t *f = call->fixture;
    uint64_t flags = lock_fixture(f);
    if (call->op == 0) {
        call->result = PW_COMMIT(&f->view, call->seq, &call->reason);
    } else if (call->op == 1) {
        call->result = PW_WAKE(&f->view, call->seq, A20_WAKE_TIMEOUT,
                               NULL, NULL);
    } else if (call->op == 2) {
        call->result = PW_CANCEL(&f->view, call->seq, NULL, NULL);
    } else {
        call->result = PW_WAKE(&f->view, call->seq, A20_WAKE_TASK_EXIT,
                               NULL, NULL);
    }
    unlock_fixture(f, flags);
    return NULL;
}

static int run_race(fixture_t *f, uint64_t seq, int op1, int op2,
                    race_call_t *a, race_call_t *b)
{
    pthread_t t1, t2;
    *a = (race_call_t){.fixture = f, .seq = seq, .op = op1, .result = -99};
    *b = (race_call_t){.fixture = f, .seq = seq, .op = op2, .result = -99};
    if (pthread_create(&t1, NULL, race_transition, a) != 0 ||
        pthread_create(&t2, NULL, race_transition, b) != 0)
        abort();
    pthread_join(t1, NULL);
    pthread_join(t2, NULL);
    return 0;
}

static int test_smp_transition_races(void)
{
    for (unsigned i = 0; i < 128; i++) {
        fixture_t f;
        fixture_init(&f);
        uint64_t seq = 0;
        CHECK(prepare(&f, A20_WAIT_INTERRUPTIBLE, 500, &seq) == 0);
        race_call_t commit, wake;
        run_race(&f, seq, 0, 1, &commit, &wake);
        CHECK((commit.result == 0 || commit.result == 1) &&
              (wake.result == 1 || wake.result == 2));
        CHECK(f.state == A20_PARK_WOKEN && f.reason == A20_WAKE_TIMEOUT);
        CHECK(PW_REASON(&f.view, seq) == A20_WAKE_TIMEOUT);
        CHECK(finish(&f, seq, 0) == 1 && f.state == A20_PARK_IDLE);
        pthread_mutex_destroy(&f.lock);
    }

    for (unsigned i = 0; i < 128; i++) {
        fixture_t f;
        fixture_init(&f);
        uint64_t seq = 0;
        CHECK(prepare(&f, A20_WAIT_KILLABLE, 600, &seq) == 0);
        race_call_t cancel, exit_wake;
        run_race(&f, seq, 2, 3, &cancel, &exit_wake);
        CHECK((cancel.result == 0 || cancel.result == 1) &&
              (exit_wake.result == 0 || exit_wake.result == 1));
        CHECK(cancel.result + exit_wake.result == 1);
        CHECK(f.state == A20_PARK_WOKEN &&
              (f.reason == A20_WAKE_CANCEL || f.reason == A20_WAKE_TASK_EXIT));
        CHECK(finish(&f, seq, 0) == 1 && f.state == A20_PARK_IDLE);
        pthread_mutex_destroy(&f.lock);
    }
    return 0;
}

int main(void)
{
    if (test_prepare_rollback_and_wrap() ||
        test_reject_invalid_api_arguments() ||
        test_wake_commit_timeout_exit_and_finish() ||
        test_cancel_rules_and_stale_tokens() ||
        test_smp_transition_races())
        return 1;
    printf("PARK_WAIT_HOST: PASS checks=%u\n", checks);
    return 0;
}

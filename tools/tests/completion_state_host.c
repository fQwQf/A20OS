#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    void *task;
    uint64_t seq;
    int prepare_error;
} wait_token_t;

struct wait_entry {
    struct wait_entry *next;
    struct wait_entry *prev;
    void *queue;
    void *task;
    uint64_t wait_seq;
    uint32_t flags;
    uintptr_t key;
    void *priv;
    _Bool linked;
};

void a20_completion_state_init_rust(unsigned *done);
void a20_completion_state_add_rust(unsigned *done);
void a20_completion_state_all_rust(unsigned *done);
void a20_completion_wait_rust(void *completion);

static unsigned done;
static unsigned prepares, commits, cancels, finishes, links, unlinks, yields;
static int prepare_fails, link_fails, wake_on_commit;

uint64_t a20_completion_lock(void *completion) { (void)completion; return 0; }
void a20_completion_unlock(void *completion, uint64_t flags)
{ (void)completion; (void)flags; }
unsigned *a20_completion_done_ptr(void *completion)
{ (void)completion; return &done; }
void *a20_completion_waiters(void *completion)
{ (void)completion; return (void *)1; }

wait_token_t proc_park_prepare(int mode, uint64_t deadline)
{
    (void)mode; (void)deadline;
    prepares++;
    return (wait_token_t){prepare_fails ? NULL : (void *)2, prepares, 0};
}
int proc_park_cancel(wait_token_t token)
{ (void)token; cancels++; return 0; }
int proc_park_commit(wait_token_t token)
{
    (void)token; commits++;
    if (wake_on_commit)
        a20_completion_state_add_rust(&done);
    return 1;
}
void proc_park_finish(wait_token_t token) { (void)token; finishes++; }
void proc_yield(void)
{
    yields++;
    a20_completion_state_add_rust(&done);
}
_Bool wait_queue_link(void *queue, struct wait_entry *entry,
                      wait_token_t token, uintptr_t key)
{
    (void)queue; (void)entry; (void)token; (void)key;
    links++;
    if (link_fails) {
        /* Simulate the condition becoming persistent before a failed link. */
        a20_completion_state_add_rust(&done);
        return 0;
    }
    return 1;
}
void wait_queue_unlink(void *queue, struct wait_entry *entry)
{ (void)queue; (void)entry; unlinks++; }

static void reset(void)
{
    done = prepares = commits = cancels = finishes = links = unlinks = yields = 0;
    prepare_fails = link_fails = wake_on_commit = 0;
}

static int fail(const char *name)
{
    fprintf(stderr, "COMPLETION_HOST: FAIL %s\n", name);
    return 1;
}

int main(void)
{
    int failures = 0;
    int completion;

    reset();
    a20_completion_state_init_rust(&done);
    wake_on_commit = 1;
    a20_completion_wait_rust(&completion);
    if (done != 0 || prepares != 1 || links != 1 || commits != 1 ||
        cancels != 0 || unlinks != 1 || finishes != 1)
        failures += fail("linked-wake-consumes-one-token-and-finishes");

    reset();
    done = 1;
    a20_completion_wait_rust(&completion);
    if (done != 0 || prepares || links || finishes)
        failures += fail("stored-completion-consumed-without-parking");

    reset();
    a20_completion_state_all_rust(&done);
    a20_completion_wait_rust(&completion);
    a20_completion_wait_rust(&completion);
    if (done != UINT32_MAX || prepares || links)
        failures += fail("complete-all-remains-persistent");

    reset();
    link_fails = 1;
    a20_completion_wait_rust(&completion);
    if (done != 0 || prepares != 1 || links != 1 || commits || cancels != 1 ||
        unlinks != 1 || finishes != 1)
        failures += fail("failed-link-cancels-and-finishes-token");

    reset();
    prepare_fails = 1;
    a20_completion_wait_rust(&completion);
    if (done != 0 || prepares != 1 || yields != 1 || links || finishes)
        failures += fail("prepare-failure-yields-and-rechecks");

    if (failures)
        return 1;
    puts("COMPLETION_HOST: PASS");
    return 0;
}

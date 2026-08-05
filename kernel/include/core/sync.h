#ifndef _CORE_SYNC_H
#define _CORE_SYNC_H

#include "core/lock.h"
#include "core/types.h"
#include "proc/park.h"

typedef struct wait_queue_entry {
    struct wait_queue_entry *next;
    struct wait_queue_entry *prev;
    struct task_t *task;
    uint64_t wait_seq;
    uint32_t flags;
    uintptr_t key;
    bool linked;
} wait_queue_entry_t;

typedef struct wait_queue {
    spinlock_t lock;
    wait_queue_entry_t *head;
} wait_queue_t;

#define WAIT_QUEUE_INIT { SPINLOCK_INIT, NULL }

/*
 * WAIT_QUEUE_PARK_PROTOCOL:
 * - The caller creates one park token before acquiring the object lock.
 * - While holding the object lock it rechecks the persistent condition and
 *   links one or more entries with wait_queue_link().
 * - A waker detaches the stack entry and copies task + wait_seq while holding
 *   q->lock, clears the entry, releases q->lock, then calls proc_try_wake().
 * - The waiter commits after dropping the object lock, unlinks every remaining
 *   entry after wake, rechecks the condition under its object lock, and then
 *   finishes the token.
 * - If the condition became true before link/commit, the caller explicitly
 *   cancels and finishes the token.
 */

void wait_queue_init(wait_queue_t *q);
bool wait_queue_link(wait_queue_t *q, wait_queue_entry_t *entry,
                     proc_wait_token_t token, uintptr_t key);
void wait_queue_unlink(wait_queue_t *q, wait_queue_entry_t *entry);
unsigned wait_queue_wake_one(wait_queue_t *q, uintptr_t key,
                             proc_wake_reason_t reason);
struct task_t *wait_queue_peek_key(wait_queue_t *q, uintptr_t key);
unsigned wait_queue_wake_all(wait_queue_t *q, uintptr_t key,
                             proc_wake_reason_t reason);
unsigned wait_queue_collect_one(wait_queue_t *q, uintptr_t key,
                                proc_wake_reason_t reason,
                                proc_wake_q_t *wake_q);
unsigned wait_queue_collect_all(wait_queue_t *q, uintptr_t key,
                                proc_wake_reason_t reason,
                                proc_wake_q_t *wake_q,
                                bool *complete);

typedef struct mutex {
    spinlock_t lock;
    int locked;
    void *owner;
    wait_queue_t waiters;
} mutex_t;

#define MUTEX_INIT { SPINLOCK_INIT, 0, NULL, WAIT_QUEUE_INIT }

void mutex_init(mutex_t *m);
int  mutex_trylock(mutex_t *m);
void mutex_lock(mutex_t *m);
void mutex_unlock(mutex_t *m);

typedef struct completion {
    spinlock_t lock;
    unsigned done;
    wait_queue_t waiters;
} completion_t;

#define COMPLETION_INIT { SPINLOCK_INIT, 0, WAIT_QUEUE_INIT }

void completion_init(completion_t *c);
void complete(completion_t *c);
void complete_all(completion_t *c);
void wait_for_completion(completion_t *c);

#endif

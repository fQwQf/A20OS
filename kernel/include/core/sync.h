#ifndef _CORE_SYNC_H
#define _CORE_SYNC_H

#include "core/lock.h"
#include "core/types.h"

typedef struct wait_queue_entry {
    struct wait_queue_entry *next;
    struct wait_queue_entry *prev;
    void *task;
} wait_queue_entry_t;

typedef struct wait_queue {
    spinlock_t lock;
    wait_queue_entry_t *head;
} wait_queue_t;

#define WAIT_QUEUE_INIT { SPINLOCK_INIT, NULL }

void wait_queue_init(wait_queue_t *q);
void wait_queue_prepare(wait_queue_t *q, wait_queue_entry_t *entry);
void wait_queue_finish(wait_queue_t *q, wait_queue_entry_t *entry);
void wait_queue_sleep(wait_queue_t *q);
void wait_queue_wake_one(wait_queue_t *q);
void wait_queue_wake_all(wait_queue_t *q);

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

#include "core/sync.h"

#include "core/consts.h"
#include "proc/proc.h"

#define COMPLETION_DONE_ALL ((unsigned)-1)

void wait_queue_init(wait_queue_t *q) {
    if (!q)
        return;
    spin_init(&q->lock);
    q->head = NULL;
}

void wait_queue_prepare(wait_queue_t *q, wait_queue_entry_t *entry) {
    task_t *cur = proc_current();
    if (!q || !entry || !cur)
        return;

    entry->task = cur;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    for (wait_queue_entry_t *e = q->head; e; e = e->next) {
        if (e == entry || e->task == cur) {
            spin_unlock_irqrestore(&q->lock, flags);
            return;
        }
    }
    entry->next = q->head;
    entry->prev = NULL;
    if (q->head)
        q->head->prev = entry;
    q->head = entry;
    spin_unlock_irqrestore(&q->lock, flags);
}

void wait_queue_finish(wait_queue_t *q, wait_queue_entry_t *entry) {
    if (!q || !entry)
        return;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    if (entry->prev)
        entry->prev->next = entry->next;
    else if (q->head == entry)
        q->head = entry->next;
    if (entry->next)
        entry->next->prev = entry->prev;
    entry->next = NULL;
    entry->prev = NULL;
    entry->task = NULL;
    spin_unlock_irqrestore(&q->lock, flags);
}

void wait_queue_sleep(wait_queue_t *q) {
    task_t *cur = proc_current();
    if (!q || !cur)
        return;

    wait_queue_entry_t entry = {0};
    entry.task = cur;

    /* Enqueue entry and set BLOCKED atomically under q->lock.
     * We inline the prepare logic to avoid recursive locking. */
    uint64_t flags = spin_lock_irqsave(&q->lock);
    for (wait_queue_entry_t *e = q->head; e; e = e->next) {
        if (e->task == cur) {
            /* Already enqueued — just mark blocked and go. */
            cur->state = PROC_BLOCKED;
            spin_unlock_irqrestore(&q->lock, flags);
            sched();
            return;
        }
    }
    entry.next = q->head;
    entry.prev = NULL;
    if (q->head)
        q->head->prev = &entry;
    q->head = &entry;
    cur->state = PROC_BLOCKED;
    spin_unlock_irqrestore(&q->lock, flags);

    sched();
    wait_queue_finish(q, &entry);
}

void wait_queue_wake_one(wait_queue_t *q) {
    if (!q)
        return;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t *entry = q->head;
    if (entry) {
        q->head = entry->next;
        if (q->head)
            q->head->prev = NULL;
        entry->next = NULL;
        entry->prev = NULL;
    }
    spin_unlock_irqrestore(&q->lock, flags);

    if (entry && entry->task) {
        task_t *t = (task_t *)entry->task;
        entry->task = NULL;
        if (t->state == PROC_BLOCKED)
            proc_make_ready(t);
    }
}

void wait_queue_wake_all(wait_queue_t *q) {
    if (!q)
        return;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t *list = q->head;
    q->head = NULL;
    spin_unlock_irqrestore(&q->lock, flags);

    while (list) {
        wait_queue_entry_t *next = list->next;
        if (list->task) {
            task_t *t = (task_t *)list->task;
            if (t->state == PROC_BLOCKED)
                proc_make_ready(t);
        }
        list->next = NULL;
        list->prev = NULL;
        list->task = NULL;
        list = next;
    }
}

void mutex_init(mutex_t *m) {
    if (!m)
        return;
    spin_init(&m->lock);
    m->locked = 0;
    m->owner = NULL;
    wait_queue_init(&m->waiters);
}

int mutex_trylock(mutex_t *m) {
    if (!m)
        return 0;

    task_t *cur = proc_current();
    uint64_t flags = spin_lock_irqsave(&m->lock);
    if (!m->locked) {
        m->locked = 1;
        m->owner = cur;
        spin_unlock_irqrestore(&m->lock, flags);
        return 1;
    }
    spin_unlock_irqrestore(&m->lock, flags);
    return 0;
}

void mutex_lock(mutex_t *m) {
    if (!m)
        return;
    while (!mutex_trylock(m))
        wait_queue_sleep(&m->waiters);
}

void mutex_unlock(mutex_t *m) {
    if (!m)
        return;

    uint64_t flags = spin_lock_irqsave(&m->lock);
    m->locked = 0;
    m->owner = NULL;
    spin_unlock_irqrestore(&m->lock, flags);
    wait_queue_wake_one(&m->waiters);
}

void completion_init(completion_t *c) {
    if (!c)
        return;
    spin_init(&c->lock);
    c->done = 0;
    wait_queue_init(&c->waiters);
}

void complete(completion_t *c) {
    if (!c)
        return;
    uint64_t flags = spin_lock_irqsave(&c->lock);
    c->done++;
    spin_unlock_irqrestore(&c->lock, flags);
    wait_queue_wake_one(&c->waiters);
}

void complete_all(completion_t *c) {
    if (!c)
        return;
    uint64_t flags = spin_lock_irqsave(&c->lock);
    c->done = COMPLETION_DONE_ALL;
    spin_unlock_irqrestore(&c->lock, flags);
    wait_queue_wake_all(&c->waiters);
}

void wait_for_completion(completion_t *c) {
    if (!c)
        return;

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&c->lock);
        if (c->done) {
            if (c->done != COMPLETION_DONE_ALL)
                c->done--;
            spin_unlock_irqrestore(&c->lock, flags);
            return;
        }
        spin_unlock_irqrestore(&c->lock, flags);
        wait_queue_sleep(&c->waiters);
    }
}

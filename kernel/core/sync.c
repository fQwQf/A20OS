#include "core/sync.h"

#include "core/consts.h"
#include "proc/proc.h"

#define COMPLETION_DONE_ALL ((unsigned)-1)

void wait_queue_init(wait_queue_t *q) {
    if (!q)
        return;
    spin_init(&q->lock);
    spin_set_debug(&q->lock, "wait_queue", q);
    q->head = NULL;
}

static void wait_queue_entry_clear(wait_queue_entry_t *entry)
{
    entry->next = NULL;
    entry->prev = NULL;
    entry->task = NULL;
    entry->wait_seq = 0;
    entry->flags = 0;
    entry->key = 0;
    entry->linked = false;
}

bool wait_queue_link(wait_queue_t *q, wait_queue_entry_t *entry,
                     proc_wait_token_t token, uintptr_t key) {
    if (!q || !entry || !token.task || !token.seq)
        return false;
    task_t *task = proc_get(token.task);
    if (!task)
        return false;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    if (entry->linked) {
        spin_unlock_irqrestore(&q->lock, flags);
        proc_put(task);
        return false;
    }
    entry->task = task;
    entry->wait_seq = token.seq;
    entry->flags = 0;
    entry->key = key;
    entry->next = q->head;
    entry->prev = NULL;
    if (q->head)
        q->head->prev = entry;
    q->head = entry;
    entry->linked = true;
    spin_unlock_irqrestore(&q->lock, flags);
    return true;
}

void wait_queue_unlink(wait_queue_t *q, wait_queue_entry_t *entry) {
    if (!q || !entry)
        return;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    task_t *task = NULL;
    if (entry->linked) {
        task = entry->task;
        if (entry->prev)
            entry->prev->next = entry->next;
        else if (q->head == entry)
            q->head = entry->next;
        if (entry->next)
            entry->next->prev = entry->prev;
    }
    wait_queue_entry_clear(entry);
    spin_unlock_irqrestore(&q->lock, flags);
    proc_put(task);
}

unsigned wait_queue_wake_one(wait_queue_t *q, uintptr_t key,
                             proc_wake_reason_t reason) {
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    (void)wait_queue_collect_one(q, key, reason, &wake_q);
    return proc_wake_q_flush(&wake_q);
}

unsigned wait_queue_wake_all(wait_queue_t *q, uintptr_t key,
                             proc_wake_reason_t reason) {
    unsigned woke = 0;
    for (;;) {
        proc_wake_q_t wake_q;
        proc_wake_q_init(&wake_q);
        bool complete = false;
        unsigned collected =
            wait_queue_collect_all(q, key, reason, &wake_q, &complete);
        if (!collected)
            break;
        woke += proc_wake_q_flush(&wake_q);
        if (complete)
            break;
    }
    return woke;
}

static void wait_queue_detach_locked(wait_queue_t *q,
                                     wait_queue_entry_t *entry)
{
    if (entry->prev)
        entry->prev->next = entry->next;
    else
        q->head = entry->next;
    if (entry->next)
        entry->next->prev = entry->prev;
    /*
     * The entry's task reference is transferred to proc_wake_q_add() before
     * this helper clears the stack entry.
     */
    wait_queue_entry_clear(entry);
}

unsigned wait_queue_collect_one(wait_queue_t *q, uintptr_t key,
                                proc_wake_reason_t reason,
                                proc_wake_q_t *wake_q)
{
    if (!q || !wake_q || wake_q->count >= PROC_WAKE_Q_CAPACITY)
        return 0;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t *entry = q->head;
    while (entry && key && entry->key != key)
        entry = entry->next;
    unsigned collected = 0;
    if (entry && proc_wake_q_add(wake_q, entry->task, entry->wait_seq,
                                 reason)) {
        wait_queue_detach_locked(q, entry);
        collected = 1;
    }
    spin_unlock_irqrestore(&q->lock, flags);
    return collected;
}

unsigned wait_queue_collect_all(wait_queue_t *q, uintptr_t key,
                                proc_wake_reason_t reason,
                                proc_wake_q_t *wake_q,
                                bool *complete)
{
    if (complete)
        *complete = false;
    if (!q || !wake_q)
        return 0;

    unsigned collected = 0;
    bool drained = true;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t *entry = q->head;
    while (entry) {
        wait_queue_entry_t *next = entry->next;
        if (!key || entry->key == key) {
            if (!proc_wake_q_add(wake_q, entry->task, entry->wait_seq,
                                 reason)) {
                drained = false;
                break;
            }
            wait_queue_detach_locked(q, entry);
            collected++;
        }
        entry = next;
    }
    spin_unlock_irqrestore(&q->lock, flags);
    if (complete)
        *complete = drained;
    return collected;
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

    task_t *cur = proc_current();
    if (!cur) {
        while (!mutex_trylock(m))
            proc_yield();
        return;
    }

    for (;;) {
        uint64_t flags = spin_lock_irqsave(&m->lock);
        if (!m->locked) {
            m->locked = 1;
            m->owner = cur;
            spin_unlock_irqrestore(&m->lock, flags);
            return;
        }
        spin_unlock_irqrestore(&m->lock, flags);

        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        if (!token.task) {
            proc_yield();
            continue;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&m->lock);
        if (!m->locked) {
            m->locked = 1;
            m->owner = cur;
            spin_unlock_irqrestore(&m->lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return;
        }
        bool linked = wait_queue_link(&m->waiters, &entry, token, 0);
        spin_unlock_irqrestore(&m->lock, flags);

        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&m->waiters, &entry);
        proc_park_finish(token);
    }
}

void mutex_unlock(mutex_t *m) {
    if (!m)
        return;

    uint64_t flags = spin_lock_irqsave(&m->lock);
    m->locked = 0;
    m->owner = NULL;
    spin_unlock_irqrestore(&m->lock, flags);
    wait_queue_wake_one(&m->waiters, 0, PROC_WAKE_EVENT);
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
    wait_queue_wake_one(&c->waiters, 0, PROC_WAKE_EVENT);
}

void complete_all(completion_t *c) {
    if (!c)
        return;
    uint64_t flags = spin_lock_irqsave(&c->lock);
    c->done = COMPLETION_DONE_ALL;
    spin_unlock_irqrestore(&c->lock, flags);
    wait_queue_wake_all(&c->waiters, 0, PROC_WAKE_EVENT);
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

        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        if (!token.task) {
            proc_yield();
            continue;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&c->lock);
        if (c->done) {
            if (c->done != COMPLETION_DONE_ALL)
                c->done--;
            spin_unlock_irqrestore(&c->lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return;
        }
        bool linked = wait_queue_link(&c->waiters, &entry, token, 0);
        spin_unlock_irqrestore(&c->lock, flags);

        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&c->waiters, &entry);
        proc_park_finish(token);
    }
}

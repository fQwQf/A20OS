#include "core/sync.h"

#include "core/consts.h"
#include "proc/proc.h"
#include "proc/lifetime.h"

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
    entry->priv = NULL;
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
    proc_lifetime_note_wait_add();
    spin_unlock_irqrestore(&q->lock, flags);
    return true;
}

bool wait_queue_link_locked(wait_queue_t *q, wait_queue_entry_t *entry,
                            proc_wait_token_t token, uintptr_t key) {
    if (!q || !entry || !token.task || !token.seq)
        return false;
    task_t *task = proc_get(token.task);
    if (!task)
        return false;
    if (entry->linked) {
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
    proc_lifetime_note_wait_add();
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
    if (task)
        proc_lifetime_note_wait_remove();
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

/*
 * wait_queue_peek_key — return (with a live reference) the task of the
 * first entry matching @key, without unlinking it.  Used by the IPC
 * donation path (docs/hybrid-kernel/02-mainstream-plan.md M1), which
 * re-validates the target's park state under proc_lock before switching.
 */
task_t *wait_queue_peek_key(wait_queue_t *q, uintptr_t key) {
    if (!q)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t *entry = q->head;
    while (entry && key && entry->key != key)
        entry = entry->next;
    task_t *task = entry ? proc_get(entry->task) : NULL;
    spin_unlock_irqrestore(&q->lock, flags);
    return task;
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
        proc_lifetime_note_wait_to_wake();
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
            proc_lifetime_note_wait_to_wake();
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

unsigned wait_queue_collect_matching(wait_queue_t *q,
                                     wait_queue_match_fn match, void *arg,
                                     unsigned limit,
                                     proc_wake_reason_t reason,
                                     proc_wake_q_t *wake_q,
                                     bool *complete)
{
    if (complete)
        *complete = false;
    if (!q || !wake_q || !match || limit == 0)
        return 0;

    unsigned collected = 0;
    bool drained = true;
    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t *entry = q->head;
    while (entry) {
        wait_queue_entry_t *next = entry->next;
        if (match(entry, arg)) {
            if (!proc_wake_q_add(wake_q, entry->task, entry->wait_seq,
                                 reason)) {
                drained = false;
                break;
            }
            proc_lifetime_note_wait_to_wake();
            wait_queue_detach_locked(q, entry);
            collected++;
            if (collected >= limit) {
                drained = false;
                break;
            }
        }
        entry = next;
    }
    spin_unlock_irqrestore(&q->lock, flags);
    if (complete)
        *complete = drained;
    return collected;
}

unsigned wait_queue_requeue_matching(wait_queue_t *q_from, wait_queue_t *q_to,
                                     wait_queue_match_fn match, void *arg,
                                     unsigned limit,
                                     wait_queue_rekey_fn rekey,
                                     void *rekey_arg)
{
    if (!q_from || !q_to || !match || limit == 0)
        return 0;

    if (q_from == q_to) {
        unsigned n = 0;
        uint64_t flags = spin_lock_irqsave(&q_from->lock);
        for (wait_queue_entry_t *entry = q_from->head;
             entry && n < limit; entry = entry->next) {
            if (match(entry, arg))
                n++;
        }
        spin_unlock_irqrestore(&q_from->lock, flags);
        return n;
    }

    /*
     * Take both queue locks in address order (the queues live in one array
     * in the futex case) so a pair of REQUEUEs cannot deadlock; the caller
     * guarantees both queues are already the same hash family.
     */
    wait_queue_t *first = q_from < q_to ? q_from : q_to;
    wait_queue_t *second = q_from < q_to ? q_to : q_from;

    wait_queue_entry_t *moved[PROC_WAKE_Q_CAPACITY];
    unsigned n = 0;

    uint64_t f1 = spin_lock_irqsave(&first->lock);
    uint64_t f2 = spin_lock_irqsave(&second->lock);

    wait_queue_entry_t **pp = &q_from->head;
    while (*pp && n < limit) {
        wait_queue_entry_t *entry = *pp;
        if (!match(entry, arg)) {
            pp = &entry->next;
            continue;
        }
        *pp = entry->next;
        if (entry->next)
            entry->next->prev = entry->prev;
        moved[n++] = entry;
    }

    for (unsigned i = 0; i < n; i++) {
        wait_queue_entry_t *entry = moved[i];
        if (rekey)
            rekey(entry, rekey_arg);
        entry->prev = NULL;
        entry->next = q_to->head;
        if (q_to->head)
            q_to->head->prev = entry;
        q_to->head = entry;
    }

    spin_unlock_irqrestore(&second->lock, f2);
    spin_unlock_irqrestore(&first->lock, f1);
    return n;
}

unsigned wait_queue_purge_task(wait_queue_t *q, struct task_t *task)
{
    if (!q || !task)
        return 0;

    task_t *removed[PROC_WAKE_Q_CAPACITY];
    unsigned n = 0;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    wait_queue_entry_t **pp = &q->head;
    while (*pp) {
        wait_queue_entry_t *entry = *pp;
        if (entry->task != task) {
            pp = &entry->next;
            continue;
        }
        removed[n] = entry->task;
        *pp = entry->next;
        if (entry->next)
            entry->next->prev = entry->prev;
        wait_queue_entry_clear(entry);
        proc_lifetime_note_wait_remove();
        n++;
        if (n >= PROC_WAKE_Q_CAPACITY)
            break;
    }
    spin_unlock_irqrestore(&q->lock, flags);

    for (unsigned i = 0; i < n; i++)
        proc_put(removed[i]);
    return n;
}

unsigned wait_queue_purge_task_locked(wait_queue_t *q, struct task_t *task)
{
    if (!q || !task)
        return 0;

    unsigned n = 0;
    wait_queue_entry_t **pp = &q->head;
    while (*pp) {
        wait_queue_entry_t *entry = *pp;
        if (entry->task != task) {
            pp = &entry->next;
            continue;
        }
        *pp = entry->next;
        if (entry->next)
            entry->next->prev = entry->prev;
        task_t *t = entry->task;
        wait_queue_entry_clear(entry);
        proc_lifetime_note_wait_remove();
        proc_put(t);
        n++;
    }
    return n;
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

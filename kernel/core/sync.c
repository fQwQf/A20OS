#include "core/sync.h"

#include "core/consts.h"
#include "core/panic.h"
#include "proc/proc.h"
#include "proc/lifetime.h"

#define COMPLETION_DONE_ALL ((unsigned)-1)

#ifdef CONFIG_COMPLETION_RUST
extern void a20_completion_state_init_rust(unsigned *done);
extern void a20_completion_state_add_rust(unsigned *done);
extern void a20_completion_state_all_rust(unsigned *done);
extern void a20_completion_wait_rust(void *completion);

uint64_t a20_completion_lock(void *completion)
{
    completion_t *c = completion;
    return c ? spin_lock_irqsave(&c->lock) : 0;
}

void a20_completion_unlock(void *completion, uint64_t flags)
{
    completion_t *c = completion;
    if (c)
        spin_unlock_irqrestore(&c->lock, flags);
}

unsigned *a20_completion_done_ptr(void *completion)
{
    completion_t *c = completion;
    return c ? &c->done : NULL;
}

wait_queue_t *a20_completion_waiters(void *completion)
{
    completion_t *c = completion;
    return c ? &c->waiters : NULL;
}
#endif

#ifdef CONFIG_WAIT_QUEUE_RUST
extern bool a20_wait_queue_insert_rust(wait_queue_entry_t **head,
                                       void *queue,
                                       wait_queue_entry_t *entry,
                                       task_t *task, uint64_t seq,
                                       uintptr_t key);
extern task_t *a20_wait_queue_detach_rust(wait_queue_entry_t **head,
                                          void *queue,
                                          wait_queue_entry_t *entry);
extern bool a20_wait_queue_requeue_rust(wait_queue_entry_t **from_head,
                                        wait_queue_entry_t **to_head,
                                        void *from, void *to,
                                        wait_queue_entry_t *entry);
#endif

#ifdef CONFIG_RW_MUTEX_RUST
extern void a20_rw_mutex_state_init_rust(rw_mutex_state_t *state);
extern int a20_rw_mutex_read_admit_rust(rw_mutex_state_t *state);
extern int a20_rw_mutex_read_release_rust(rw_mutex_state_t *state);
extern void a20_rw_mutex_writer_wait_rust(rw_mutex_state_t *state);
extern int a20_rw_mutex_write_admit_rust(rw_mutex_state_t *state,
                                         void *owner);
extern int a20_rw_mutex_write_release_rust(rw_mutex_state_t *state);
#endif

#ifdef CONFIG_MUTEX_STATE_RUST
_Static_assert(__builtin_offsetof(mutex_t, depth) -
                   __builtin_offsetof(mutex_t, locked) == sizeof(int),
               "Rust mutex state depth offset mismatch");
_Static_assert(__builtin_offsetof(mutex_t, owner) -
                   __builtin_offsetof(mutex_t, locked) == 2 * sizeof(int),
               "Rust mutex state owner offset mismatch");
_Static_assert(__alignof__(mutex_t) >= __alignof__(int),
               "Rust mutex state alignment mismatch");
extern void a20_mutex_state_init_rust(void *state);
extern int a20_mutex_try_acquire_rust(void *state, void *owner);
extern int a20_mutex_reenter_rust(void *state, void *owner);
extern int a20_mutex_release_rust(void *state);
#endif

void wait_queue_init(wait_queue_t *q) {
    if (!q)
        return;
    spin_init(&q->lock);
    spin_set_debug(&q->lock, "wait_queue", q);
    q->head = NULL;
    q->generation = 0;
}

uint64_t wait_queue_generation(wait_queue_t *q)
{
    if (!q)
        return 0;
    return __atomic_load_n(&q->generation, __ATOMIC_ACQUIRE);
}

#ifndef CONFIG_WAIT_QUEUE_RUST
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
    /* Publish detachment only after every field is no longer touched.  A
     * timeout-side unlink may free its containing waiter as soon as it sees
     * a NULL owner. */
    __atomic_store_n(&entry->queue, NULL, __ATOMIC_RELEASE);
}
#endif

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
    bool linked;
#ifdef CONFIG_WAIT_QUEUE_RUST
    linked = a20_wait_queue_insert_rust(&q->head, q, entry, task,
                                        token.seq, key);
#else
    linked = !entry->linked;
    if (linked) {
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
        __atomic_store_n(&entry->queue, q, __ATOMIC_RELEASE);
    }
#endif
    if (!linked) {
        spin_unlock_irqrestore(&q->lock, flags);
        proc_put(task);
        return false;
    }
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
#ifdef CONFIG_WAIT_QUEUE_RUST
    if (!a20_wait_queue_insert_rust(&q->head, q, entry, task,
                                    token.seq, key)) {
        proc_put(task);
        return false;
    }
#else
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
    __atomic_store_n(&entry->queue, q, __ATOMIC_RELEASE);
#endif
    proc_lifetime_note_wait_add();
    return true;
}

void wait_queue_unlink(wait_queue_t *q, wait_queue_entry_t *entry) {
    if (!entry)
        return;

    /*
     * REQUEUE can move an entry after its waiter drops the original object
     * lock.  Follow the entry's current owner instead of trusting the queue
     * that was used at link time.  Revalidate after taking the owner lock so
     * a concurrent move either completes first or retries here.
     */
    (void)q;
    for (;;) {
        wait_queue_t *owner =
            __atomic_load_n(&entry->queue, __ATOMIC_ACQUIRE);
        if (!owner)
            return;

        uint64_t flags = spin_lock_irqsave(&owner->lock);
        if (__atomic_load_n(&entry->queue, __ATOMIC_RELAXED) != owner) {
            spin_unlock_irqrestore(&owner->lock, flags);
            continue;
        }

        task_t *task = NULL;
#ifdef CONFIG_WAIT_QUEUE_RUST
        task = a20_wait_queue_detach_rust(&owner->head, owner, entry);
#else
        if (entry->linked) {
            task = entry->task;
            if (entry->prev)
                entry->prev->next = entry->next;
            else if (owner->head == entry)
                owner->head = entry->next;
            if (entry->next)
                entry->next->prev = entry->prev;
        }
#endif
        if (task)
            proc_lifetime_note_wait_remove();
#ifndef CONFIG_WAIT_QUEUE_RUST
        wait_queue_entry_clear(entry);
#endif
        spin_unlock_irqrestore(&owner->lock, flags);
        proc_put(task);
        return;
    }
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
 * donation path (docs/zh/hybrid-kernel/02-mainstream-plan.md M1), which
 * re-validates the target's park state under the target's park_lock before
 * switching.
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
#ifdef CONFIG_WAIT_QUEUE_RUST
    (void)a20_wait_queue_detach_rust(&q->head, q, entry);
#else
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
#endif
}

unsigned wait_queue_collect_one(wait_queue_t *q, uintptr_t key,
                                proc_wake_reason_t reason,
                                proc_wake_q_t *wake_q)
{
    if (!q || !wake_q || wake_q->count >= PROC_WAKE_Q_CAPACITY)
        return 0;

    uint64_t flags = spin_lock_irqsave(&q->lock);
    __atomic_add_fetch(&q->generation, 1, __ATOMIC_RELEASE);
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
    __atomic_add_fetch(&q->generation, 1, __ATOMIC_RELEASE);
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

unsigned wait_queue_collect_matching_locked(wait_queue_t *q,
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
    if (!q) {
        if (complete) *complete = false;
        return 0;
    }
    uint64_t flags = spin_lock_irqsave(&q->lock);
    unsigned collected = wait_queue_collect_matching_locked(q, match, arg,
                                       limit, reason, wake_q, complete);
    spin_unlock_irqrestore(&q->lock, flags);
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
            if (match(entry, arg)) {
                if (rekey)
                    rekey(entry, rekey_arg);
                n++;
            }
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

    unsigned n = 0;

    uint64_t f1 = spin_lock_irqsave(&first->lock);
    uint64_t f2 = spin_lock_irqsave(&second->lock);

#ifdef CONFIG_WAIT_QUEUE_RUST
    for (wait_queue_entry_t *entry = q_from->head;
         entry && n < limit;) {
        wait_queue_entry_t *next = entry->next;
        if (match(entry, arg)) {
            if (rekey)
                rekey(entry, rekey_arg);
            if (a20_wait_queue_requeue_rust(&q_from->head, &q_to->head,
                                            q_from, q_to, entry))
                n++;
        }
        entry = next;
    }
#else
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
        if (rekey)
            rekey(entry, rekey_arg);
        entry->prev = NULL;
        entry->next = q_to->head;
        if (q_to->head)
            q_to->head->prev = entry;
        q_to->head = entry;
        __atomic_store_n(&entry->queue, q_to, __ATOMIC_RELEASE);
        n++;
    }
#endif

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
#ifdef CONFIG_WAIT_QUEUE_RUST
    for (wait_queue_entry_t *entry = q->head;
         entry && n < PROC_WAKE_Q_CAPACITY;) {
        wait_queue_entry_t *next = entry->next;
        if (entry->task == task) {
            task_t *removed_task = a20_wait_queue_detach_rust(&q->head, q,
                                                               entry);
            if (removed_task) {
                removed[n++] = removed_task;
                proc_lifetime_note_wait_remove();
            }
        }
        entry = next;
    }
#else
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
#endif
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
#ifdef CONFIG_WAIT_QUEUE_RUST
    for (wait_queue_entry_t *entry = q->head; entry;) {
        wait_queue_entry_t *next = entry->next;
        if (entry->task == task) {
            task_t *t = a20_wait_queue_detach_rust(&q->head, q, entry);
            if (t) {
                proc_lifetime_note_wait_remove();
                proc_put(t);
                n++;
            }
        }
        entry = next;
    }
#else
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
#endif
    return n;
}

void mutex_init(mutex_t *m) {
    if (!m)
        return;
    spin_init(&m->lock);
#ifdef CONFIG_MUTEX_STATE_RUST
    a20_mutex_state_init_rust(&m->locked);
#else
    m->locked = 0;
    m->depth = 0;
    m->owner = NULL;
#endif
    wait_queue_init(&m->waiters);
}

int mutex_trylock(mutex_t *m) {
    if (!m)
        return 0;

    task_t *cur = proc_current();
    uint64_t flags = spin_lock_irqsave(&m->lock);
    int acquired;
#ifdef CONFIG_MUTEX_STATE_RUST
    acquired = a20_mutex_try_acquire_rust(&m->locked, cur);
#else
    if (!m->locked) {
        m->locked = 1;
        m->depth = 1;
        m->owner = cur;
        acquired = 1;
    } else {
        acquired = 0;
    }
#endif
    spin_unlock_irqrestore(&m->lock, flags);
    return acquired;
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
        /* Recursive acquisition by the same task.  This is a workaround for a
         * layering violation, not a designed feature: a VFS path re-enters a
         * vfile's lock through a different route (writeback, /proc rendering)
         * than the one that took it.  The proper fix is to drop the lock before
         * calling into the filesystem, not to make the lock reentrant -- a
         * reentrant lock here hides the reentrancy from the locking audit in
         * tools/gates.toml (check-mm-lock-model) and lets a future caller take
         * the lock twice for a real reason.  Until that layering is fixed, count
         * the depth so the reentrancy is observable and bounded rather than a
         * self-deadlock. */
        uint64_t flags = spin_lock_irqsave(&m->lock);
#ifdef CONFIG_MUTEX_STATE_RUST
        int recursive = a20_mutex_reenter_rust(&m->locked, cur);
        if (recursive < 0) {
            spin_unlock_irqrestore(&m->lock, flags);
            panic("mutex recursion depth overflow");
        }
        if (recursive) {
            spin_unlock_irqrestore(&m->lock, flags);
            return;
        }
        int acquired = a20_mutex_try_acquire_rust(&m->locked, cur);
#else
        if (m->locked && m->owner == cur) {
            m->depth++;
            spin_unlock_irqrestore(&m->lock, flags);
            return;
        }
        int acquired = 0;
        if (!m->locked) {
            m->locked = 1;
            m->depth = 1;
            m->owner = cur;
            acquired = 1;
        }
#endif
        spin_unlock_irqrestore(&m->lock, flags);
        if (acquired)
            return;

        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        if (!token.task) {
            proc_yield();
            continue;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&m->lock);
#ifdef CONFIG_MUTEX_STATE_RUST
        acquired = a20_mutex_try_acquire_rust(&m->locked, cur);
#else
        acquired = 0;
        if (!m->locked) {
            m->locked = 1;
            m->depth = 1;
            m->owner = cur;
            acquired = 1;
        }
#endif
        if (acquired) {
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
#ifdef CONFIG_MUTEX_STATE_RUST
    int became_free = a20_mutex_release_rust(&m->locked);
#else
    if (m->depth > 1) {
        m->depth--;
        spin_unlock_irqrestore(&m->lock, flags);
        return;
    }
    m->locked = 0;
    m->depth = 0;
    m->owner = NULL;
    int became_free = 1;
#endif
    spin_unlock_irqrestore(&m->lock, flags);
    if (became_free)
        wait_queue_wake_one(&m->waiters, 0, PROC_WAKE_EVENT);
}

void rw_mutex_init(rw_mutex_t *rw)
{
    if (!rw)
        return;
    spin_init(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
    a20_rw_mutex_state_init_rust(&rw->state);
#else
    rw->state.readers = 0;
    rw->state.waiting_writers = 0;
    rw->state.writer = 0;
    rw->state.owner = NULL;
#endif
    wait_queue_init(&rw->reader_waiters);
    wait_queue_init(&rw->writer_waiters);
}

void rw_mutex_read_lock(rw_mutex_t *rw)
{
    if (!rw)
        return;

    task_t *cur = proc_current();
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
        int admitted = a20_rw_mutex_read_admit_rust(&rw->state);
#else
        int admitted = !rw->state.writer && rw->state.waiting_writers == 0;
        if (admitted)
            rw->state.readers++;
#endif
        if (admitted) {
            spin_unlock_irqrestore(&rw->lock, flags);
            return;
        }
        spin_unlock_irqrestore(&rw->lock, flags);

        if (!cur) {
            proc_yield();
            continue;
        }
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        if (!token.task) {
            proc_yield();
            continue;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
        admitted = a20_rw_mutex_read_admit_rust(&rw->state);
#else
        admitted = !rw->state.writer && rw->state.waiting_writers == 0;
        if (admitted)
            rw->state.readers++;
#endif
        if (admitted) {
            spin_unlock_irqrestore(&rw->lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return;
        }
        bool linked = wait_queue_link(&rw->reader_waiters, &entry, token, 0);
        spin_unlock_irqrestore(&rw->lock, flags);
        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&rw->reader_waiters, &entry);
        proc_park_finish(token);
    }
}

void rw_mutex_read_unlock(rw_mutex_t *rw)
{
    if (!rw)
        return;
    int wake_writer;
    uint64_t flags = spin_lock_irqsave(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
    wake_writer = a20_rw_mutex_read_release_rust(&rw->state);
#else
    if (rw->state.readers > 0)
        rw->state.readers--;
    wake_writer = rw->state.readers == 0 && rw->state.waiting_writers > 0;
#endif
    spin_unlock_irqrestore(&rw->lock, flags);
    if (wake_writer)
        wait_queue_wake_one(&rw->writer_waiters, 0, PROC_WAKE_EVENT);
}

void rw_mutex_write_lock(rw_mutex_t *rw)
{
    if (!rw)
        return;

    task_t *cur = proc_current();
    uint64_t flags = spin_lock_irqsave(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
    a20_rw_mutex_writer_wait_rust(&rw->state);
#else
    rw->state.waiting_writers++;
#endif
    spin_unlock_irqrestore(&rw->lock, flags);

    for (;;) {
        flags = spin_lock_irqsave(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
        int admitted = a20_rw_mutex_write_admit_rust(&rw->state, cur);
#else
        int admitted = !rw->state.writer && rw->state.readers == 0;
        if (admitted) {
            rw->state.waiting_writers--;
            rw->state.writer = 1;
            rw->state.owner = cur;
        }
#endif
        if (admitted) {
            spin_unlock_irqrestore(&rw->lock, flags);
            return;
        }
        spin_unlock_irqrestore(&rw->lock, flags);

        if (!cur) {
            proc_yield();
            continue;
        }
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        if (!token.task) {
            proc_yield();
            continue;
        }
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
        admitted = a20_rw_mutex_write_admit_rust(&rw->state, cur);
#else
        admitted = !rw->state.writer && rw->state.readers == 0;
        if (admitted) {
            rw->state.waiting_writers--;
            rw->state.writer = 1;
            rw->state.owner = cur;
        }
#endif
        if (admitted) {
            spin_unlock_irqrestore(&rw->lock, flags);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            return;
        }
        bool linked = wait_queue_link(&rw->writer_waiters, &entry, token, 0);
        spin_unlock_irqrestore(&rw->lock, flags);
        if (linked)
            (void)proc_park_commit(token);
        else
            (void)proc_park_cancel(token);
        wait_queue_unlink(&rw->writer_waiters, &entry);
        proc_park_finish(token);
    }
}

void rw_mutex_write_unlock(rw_mutex_t *rw)
{
    if (!rw)
        return;
    int wake_writer;
    uint64_t flags = spin_lock_irqsave(&rw->lock);
#ifdef CONFIG_RW_MUTEX_RUST
    wake_writer = a20_rw_mutex_write_release_rust(&rw->state);
#else
    rw->state.writer = 0;
    rw->state.owner = NULL;
    wake_writer = rw->state.waiting_writers > 0;
#endif
    spin_unlock_irqrestore(&rw->lock, flags);
    if (wake_writer)
        wait_queue_wake_one(&rw->writer_waiters, 0, PROC_WAKE_EVENT);
    else
        wait_queue_wake_all(&rw->reader_waiters, 0, PROC_WAKE_EVENT);
}

void completion_init(completion_t *c) {
    if (!c)
        return;
    spin_init(&c->lock);
#ifdef CONFIG_COMPLETION_RUST
    a20_completion_state_init_rust(&c->done);
#else
    c->done = 0;
#endif
    wait_queue_init(&c->waiters);
}

void complete(completion_t *c) {
    if (!c)
        return;
    proc_wake_q_t wake_q;
    proc_wake_q_init(&wake_q);
    uint64_t flags = spin_lock_irqsave(&c->lock);
#ifdef CONFIG_COMPLETION_RUST
    a20_completion_state_add_rust(&c->done);
#else
    c->done++;
#endif
    /* The waiter may release a stack completion as soon as it consumes done. */
    (void)wait_queue_collect_one(&c->waiters, 0, PROC_WAKE_EVENT, &wake_q);
    spin_unlock_irqrestore(&c->lock, flags);
    proc_wake_q_flush(&wake_q);
}

void complete_all(completion_t *c) {
    if (!c)
        return;
    uint64_t flags = spin_lock_irqsave(&c->lock);
#ifdef CONFIG_COMPLETION_RUST
    a20_completion_state_all_rust(&c->done);
#else
    c->done = COMPLETION_DONE_ALL;
#endif
    spin_unlock_irqrestore(&c->lock, flags);
    wait_queue_wake_all(&c->waiters, 0, PROC_WAKE_EVENT);
}

void wait_for_completion(completion_t *c) {
    if (!c)
        return;

#ifdef CONFIG_COMPLETION_RUST
    a20_completion_wait_rust(c);
#else

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
#endif
}

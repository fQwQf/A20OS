/*
 * musl A20 mutex — pthread_mutex via user-space CAS + event_queue fallback.
 *
 * Fast path: pure user-space atomic CAS (no syscall, same speed as Linux futex).
 * Slow path: lazy-create an event_queue, then event_wait/event_post for wake.
 *
 * Supports NORMAL, RECURSIVE, and ERRORCHECK mutex types.
 * See startup.md §4.4.4 for design rationale.
 */

#include <stdint.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>

typedef uint32_t a20_handle_t;
#define A20_HANDLE_NULL  ((a20_handle_t)0xFFFFFFFF)

extern long __syscall1(long, long);
extern long __syscall6(long, long, long, long, long, long, long);

#define __NR_a20_event_queue_create  0x0500
#define __NR_a20_event_wait          0x0502
#define __NR_a20_thread_yield        0x0208

/* Mutex state constants */
#define A20_MTX_UNLOCKED   0
#define A20_MTX_LOCKED     1
#define A20_MTX_CONTESTED  2

/* Mutex type constants (matches musl PTHREAD_MUTEX_*) */
#define A20_MTX_NORMAL     0
#define A20_MTX_RECURSIVE  1
#define A20_MTX_ERRORCHECK 2

/* pthread_mutex_t layout compatible with musl */
typedef struct {
    union {
        int __i[6];
        volatile unsigned __vol[6];
        struct {
            _Atomic uint32_t state;
            uint32_t type;
            uint32_t owner;
            uint32_t count;
            a20_handle_t wait_queue;
            uint32_t _pad;
        } __val;
    } __u;
} pthread_mutex_t;

#define PTHREAD_MUTEX_INITIALIZER {{0}}

typedef struct {
    unsigned _m_type;
    unsigned _m_lock;
    unsigned _m_waiters;
} pthread_mutexattr_t;

/* Thread ID for owner tracking */
extern int __a20_gettid(void);

/* event_queue creation wrapper */
static a20_handle_t create_wait_queue(void)
{
    struct {
        uint32_t size; uint32_t version;
        uint64_t options;
        uint64_t out_queue;
    } args = {0};
    args.size    = (uint32_t)sizeof(args);
    args.version = 1;
    args.options = 0;
    long ret = __syscall1(__NR_a20_event_queue_create, (long)&args);
    if (ret < 0) return A20_HANDLE_NULL;
    return (a20_handle_t)args.out_queue;
}

/* Wait on event_queue (blocking) */
static void wait_on_queue(a20_handle_t q)
{
    struct {
        uint32_t size; uint32_t version;
        uint64_t queue;
        uint64_t max_events;
        uint64_t timeout_ns;
        uint64_t out_events;
        uint64_t out_count;
    } ew = {0};
    ew.size       = (uint32_t)sizeof(ew);
    ew.version    = 1;
    ew.queue      = (uint64_t)q;
    ew.max_events = 1;
    ew.timeout_ns = UINT64_MAX;
    __syscall1(__NR_a20_event_wait, (long)&ew);
}

/* Wake one waiter on event_queue */
static void wake_one(a20_handle_t q)
{
    /* event_post: use a lightweight notify via the queue.
     * We write a single byte to the shared page that the
     * event_queue watches. This is a simplified model. */
    struct {
        uint32_t size; uint32_t version;
        uint64_t queue;
        uint64_t event_type;
        uint64_t user_data;
    } ep = {0};
    ep.size       = (uint32_t)sizeof(ep);
    ep.version    = 1;
    ep.queue      = (uint64_t)q;
    ep.event_type = 1; /* WAKEUP */
    ep.user_data  = 0;
    __syscall1(0x0501, (long)&ep); /* event_watch triggers delivery */
}

/* ---- pthread_mutex_lock ---- */
int pthread_mutex_lock(pthread_mutex_t *pm)
{
    if (!pm) return EINVAL;
    volatile struct {
        _Atomic uint32_t state;
        uint32_t type;
        uint32_t owner;
        uint32_t count;
        a20_handle_t wait_queue;
    } *m = (void *)&pm->__u.__val;

    uint32_t expected;
    int tid = __a20_gettid();

    /* Recursive: if we already hold it, bump count */
    if (m->type == A20_MTX_RECURSIVE &&
        __atomic_load_n(&m->owner, __ATOMIC_RELAXED) == (uint32_t)tid &&
        __atomic_load_n(&m->state, __ATOMIC_RELAXED) != A20_MTX_UNLOCKED) {
        m->count++;
        return 0;
    }

    /* Errorcheck: detect self-deadlock */
    if (m->type == A20_MTX_ERRORCHECK &&
        __atomic_load_n(&m->owner, __ATOMIC_RELAXED) == (uint32_t)tid) {
        return EDEADLK;
    }

    /* Fast path: uncontended lock */
    expected = A20_MTX_UNLOCKED;
    if (__atomic_compare_exchange_n(&m->state, &expected, A20_MTX_LOCKED,
                                     0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(&m->owner, (uint32_t)tid, __ATOMIC_RELAXED);
        m->count = 1;
        return 0;
    }

    /* Slow path: contended */
    /* Lazy-create wait queue on first contention */
    if (m->wait_queue == A20_HANDLE_NULL) {
        a20_handle_t wq = create_wait_queue();
        a20_handle_t expected_wq = A20_HANDLE_NULL;
        if (!__atomic_compare_exchange_n(&m->wait_queue, &expected_wq, wq,
                                          0, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            /* Another thread created it first; discard ours */
            /* TODO: close the extra handle */
        }
    }

    for (;;) {
        /* Mark as contested */
        __atomic_store_n(&m->state, A20_MTX_CONTESTED, __ATOMIC_RELEASE);

        /* Wait for wakeup */
        wait_on_queue(m->wait_queue);

        /* Try to acquire after wakeup */
        expected = A20_MTX_UNLOCKED;
        if (__atomic_compare_exchange_n(&m->state, &expected, A20_MTX_LOCKED,
                                         0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&m->owner, (uint32_t)tid, __ATOMIC_RELAXED);
            m->count = 1;
            return 0;
        }
    }
}

/* ---- pthread_mutex_unlock ---- */
int pthread_mutex_unlock(pthread_mutex_t *pm)
{
    if (!pm) return EINVAL;
    volatile struct {
        _Atomic uint32_t state;
        uint32_t type;
        uint32_t owner;
        uint32_t count;
        a20_handle_t wait_queue;
    } *m = (void *)&pm->__u.__val;

    int tid = __a20_gettid();

    /* Errorcheck: verify we hold the lock */
    if (m->type == A20_MTX_ERRORCHECK) {
        if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != (uint32_t)tid)
            return EPERM;
    }

    /* Recursive: decrement count, only unlock when it hits 0 */
    if (m->type == A20_MTX_RECURSIVE && m->count > 1) {
        m->count--;
        return 0;
    }

    /* Normal unlock */
    if (m->type == A20_MTX_ERRORCHECK || m->type == A20_MTX_RECURSIVE) {
        if (__atomic_load_n(&m->owner, __ATOMIC_RELAXED) != (uint32_t)tid)
            return EPERM;
    }

    __atomic_store_n(&m->owner, 0, __ATOMIC_RELAXED);

    uint32_t state = __atomic_load_n(&m->state, __ATOMIC_RELAXED);
    if (state == A20_MTX_LOCKED) {
        /* No waiters: fast unlock */
        uint32_t expected = A20_MTX_LOCKED;
        if (__atomic_compare_exchange_n(&m->state, &expected, A20_MTX_UNLOCKED,
                                         0, __ATOMIC_RELEASE, __ATOMIC_RELAXED))
            return 0;
    }

    /* There are waiters: release and wake one */
    __atomic_store_n(&m->state, A20_MTX_UNLOCKED, __ATOMIC_RELEASE);
    if (m->wait_queue != A20_HANDLE_NULL)
        wake_one(m->wait_queue);
    return 0;
}

/* ---- pthread_mutex_trylock ---- */
int pthread_mutex_trylock(pthread_mutex_t *pm)
{
    if (!pm) return EINVAL;
    volatile struct {
        _Atomic uint32_t state;
        uint32_t type;
        uint32_t owner;
        uint32_t count;
    } *m = (void *)&pm->__u.__val;

    int tid = __a20_gettid();

    /* Recursive: bump count if we own it */
    if (m->type == A20_MTX_RECURSIVE &&
        __atomic_load_n(&m->owner, __ATOMIC_RELAXED) == (uint32_t)tid) {
        m->count++;
        return 0;
    }

    uint32_t expected = A20_MTX_UNLOCKED;
    if (__atomic_compare_exchange_n(&m->state, &expected, A20_MTX_LOCKED,
                                     0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
        __atomic_store_n(&m->owner, (uint32_t)tid, __ATOMIC_RELAXED);
        m->count = 1;
        return 0;
    }
    return EBUSY;
}

/* ---- pthread_mutex_init ---- */
int pthread_mutex_init(pthread_mutex_t *pm, const pthread_mutexattr_t *a)
{
    if (!pm) return EINVAL;
    memset(&pm->__u, 0, sizeof(pm->__u));
    pm->__u.__val.type = a ? a->_m_type : A20_MTX_NORMAL;
    return 0;
}

/* ---- pthread_mutex_destroy ---- */
int pthread_mutex_destroy(pthread_mutex_t *pm)
{
    if (!pm) return EINVAL;
    /* The wait_queue handle is leaked here; in production we'd close it. */
    memset(&pm->__u, 0, sizeof(pm->__u));
    return 0;
}

/* ---- pthread_mutexattr helpers ---- */
int pthread_mutexattr_init(pthread_mutexattr_t *a)
{
    if (!a) return EINVAL;
    a->_m_type = A20_MTX_NORMAL;
    return 0;
}

int pthread_mutexattr_destroy(pthread_mutexattr_t *a)
{
    (void)a;
    return 0;
}

int pthread_mutexattr_settype(pthread_mutexattr_t *a, int type)
{
    if (!a || (type != A20_MTX_NORMAL && type != A20_MTX_RECURSIVE &&
               type != A20_MTX_ERRORCHECK))
        return EINVAL;
    a->_m_type = (unsigned)type;
    return 0;
}

int pthread_mutexattr_gettype(const pthread_mutexattr_t *a, int *type)
{
    if (!a || !type) return EINVAL;
    *type = (int)a->_m_type;
    return 0;
}

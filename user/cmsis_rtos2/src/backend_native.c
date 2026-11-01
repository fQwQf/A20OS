/*
 * CMSIS-RTOS2 backend for the A20OS Native ABI (NOMMU / MCU builds).
 *
 * The backend owns the futex blocking-queue protocol, thread bookkeeping and
 * stack ownership.  Every kernel interaction goes through native_platform.h,
 * which has a real A20 implementation and a host implementation used by the
 * test suite.  That split is also what makes the syscall trim checkable.
 *
 * "Self" lookup does not use TLS: this kernel keeps the current task in the
 * thread pointer, so user code cannot reserve tp for a TLS block.  Instead
 * every thread stack is registered when the thread is created and the current
 * stack pointer selects the owning thread.
 */
#include "os_impl.h"
#include "native_platform.h"

#ifndef OS_NATIVE_TICK_HZ
#define OS_NATIVE_TICK_HZ 100U
#endif

#define A20_FUTEX_FOREVER (UINT64_MAX / 4U)

struct os_bq {
    uint32_t lock;
    uint32_t seq;
};

/* ------------------------------------------------------------------------- */
/* Current-thread lookup                                                     */
/* ------------------------------------------------------------------------- */

struct self_slot {
    uintptr_t lo;
    uintptr_t hi;
    osThreadId_t id;
    struct self_slot *next;
};

static osThreadId_t     g_main_self;
static struct self_slot *g_selves;
static uint32_t         g_self_lock;

static uintptr_t current_sp(void)
{
#if defined(__riscv)
    uintptr_t sp;
    __asm__ volatile("mv %0, sp" : "=r"(sp));
    return sp;
#else
    return (uintptr_t)__builtin_frame_address(0);
#endif
}

static void self_lock(void)
{
    for (;;) {
        int exchanged = 0;
        (void)osn_atomic32(&g_self_lock, OSN_ATOMIC_CAS, 0, 1, &exchanged);
        if (exchanged)
            return;
        osn_thread_yield();
    }
}

static void self_unlock(void)
{
    (void)osn_atomic32(&g_self_lock, OSN_ATOMIC_STORE, 0, 0, NULL);
}

static void self_register(osThreadId_t t)
{
    struct self_slot *s = (struct self_slot *)osb_alloc(sizeof(*s));
    if (!s)
        return;
    s->lo = (uintptr_t)t->stack_mem;
    s->hi = s->lo + t->stack_size;
    s->id = t;
    self_lock();
    s->next = g_selves;
    g_selves = s;
    self_unlock();
}

static void self_unregister(osThreadId_t t)
{
    self_lock();
    struct self_slot **pp = &g_selves;
    while (*pp) {
        if ((*pp)->id == t) {
            struct self_slot *dead = *pp;
            *pp = dead->next;
            self_unlock();
            osb_free(dead);
            return;
        }
        pp = &(*pp)->next;
    }
    self_unlock();
}

/* ------------------------------------------------------------------------- */
/* Platform glue                                                             */
/* ------------------------------------------------------------------------- */

int osb_init(void)
{
    osn_platform_init();
    uint64_t ns;
    return osn_clock_get_ns(&ns) == 0 ? 0 : -1;
}

void *osb_alloc(size_t size)
{
    void *p = NULL;
    return osn_vm_alloc(size, &p) == 0 ? p : NULL;
}

void osb_free(void *ptr) { osn_vm_free(ptr); }

uint64_t osb_time_ns(void)
{
    uint64_t ns = 0;
    (void)osn_clock_get_ns(&ns);
    return ns;
}

uint32_t osb_tick_freq(void) { return OS_NATIVE_TICK_HZ; }

/* ------------------------------------------------------------------------- */
/* Threads                                                                   */
/* ------------------------------------------------------------------------- */

static void native_entry(void *arg)
{
    os_thread_entry(arg);
}

int osb_thread_start(osThreadId_t t)
{
    size_t size = osn_stack_grow(t->stack_size);
    if (!t->stack_mem) {
        t->stack_mem = osb_alloc(size);
        if (!t->stack_mem)
            return -OS_ENOMEM;
        t->owned_stack = t->stack_mem;
        t->stack_size = (uint32_t)size;
    } else if (size > t->stack_size) {
        /* The caller supplied a stack the platform cannot use. */
        return -OS_EINVAL;
    }
    /* Register before the thread can run: the first thing it does is look
     * itself up by stack address. */
    self_register(t);
    if (osn_thread_create(native_entry, t, t->stack_mem, t->stack_size,
                          &t->bhandle) != 0) {
        self_unregister(t);
        return -OS_EAGAIN;
    }
    return 0;
}

void osb_thread_exit(void)
{
    osThreadId_t self = osb_thread_self();
    if (self) {
        /* Join is a user-space completion word.  The kernel's wait4 model is
         * process-oriented (a sibling thread is not a waitable child), so the
         * personality owns the thread-exit edge itself. */
        self->exit_word = 1;
        (void)osn_futex_wake((uint32_t *)(uintptr_t)&self->exit_word, 0x7FFFFFFFU);
        self_unregister(self);
    }
    osn_thread_exit();
    for (;;) { }
}

int osb_thread_join(osThreadId_t t)
{
    if (!t->bhandle)
        return -OS_EINVAL;
    while (!t->exit_word)
        (void)osn_futex_wait((uint32_t *)(uintptr_t)&t->exit_word, 0,
                             A20_FUTEX_FOREVER);
    /* The completion word is set before the backend thread leaves; on the
     * host this joins the pthread so its stack cannot be freed underneath it. */
    osn_thread_reap(t->bhandle);
    (void)osn_handle_close(t->bhandle);
    t->bhandle = NULL;
    self_unregister(t);
    return 0;
}

int osb_thread_detach(osThreadId_t t)
{
    if (!t->bhandle)
        return -OS_EINVAL;
    (void)osn_handle_close(t->bhandle);
    t->bhandle = NULL;
    return 0;
}

void osb_thread_yield(void) { osn_thread_yield(); }

int osb_thread_sleep_ns(uint64_t ns)
{
    return osn_thread_sleep_ns(ns) == 0 ? 0 : -OS_EIO;
}

int osb_thread_suspend(osThreadId_t t)
{
    (void)t;
    return -OS_ENOSYS;
}

int osb_thread_resume(osThreadId_t t)
{
    (void)t;
    return -OS_ENOSYS;
}

int osb_thread_terminate(osThreadId_t t)
{
    if (!t->bhandle)
        return -OS_EINVAL;
    return osn_task_kill(t->bhandle) == 0 ? 0 : -OS_EIO;
}

osThreadId_t osb_thread_self(void)
{
    uintptr_t sp = current_sp();
    self_lock();
    for (struct self_slot *s = g_selves; s; s = s->next) {
        if (sp >= s->lo && sp <= s->hi) {
            osThreadId_t id = s->id;
            self_unlock();
            return id;
        }
    }
    self_unlock();
    return g_main_self;
}

void osb_thread_set_self(osThreadId_t t)
{
    /* Called for the main thread during kernel init and by every trampoline.
     * A trampoline already matches its registered stack, so this only takes
     * effect for the main thread. */
    if (osb_thread_self() == NULL)
        g_main_self = t;
}

int osb_thread_set_priority(osThreadId_t t, osPriority_t prio)
{
    return osn_task_set_sched(t->bhandle, OSN_SCHED_PRIORITY, (int32_t)prio, 0)
               == 0 ? 0 : -OS_EIO;
}

osPriority_t osb_thread_get_priority(osThreadId_t t)
{
    (void)t;
    return osPriorityNone;
}

int osb_thread_set_affinity(osThreadId_t t, uint32_t cpu_set)
{
    return osn_task_set_sched(t->bhandle, OSN_SCHED_AFFINITY, 0, cpu_set) == 0
               ? 0 : -OS_EIO;
}

uint32_t osb_thread_get_affinity(osThreadId_t t)
{
    (void)t;
    return 0;
}

uint32_t osb_thread_stack_space(osThreadId_t t)
{
    (void)t;
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Kernel lock (uniprocessor: recursion bookkeeping only)                    */
/* ------------------------------------------------------------------------- */

static int g_kernel_depth;

int osb_kernel_lock(void)
{
    int prev = g_kernel_depth;
    g_kernel_depth++;
    return prev;
}

int osb_kernel_unlock(void)
{
    if (g_kernel_depth > 0)
        g_kernel_depth--;
    return g_kernel_depth;
}

int osb_kernel_restore(int depth)
{
    int prev = g_kernel_depth;
    g_kernel_depth = depth;
    return prev;
}

/* ------------------------------------------------------------------------- */
/* Blocking queue: futex mutex + sequence counter                            */
/* ------------------------------------------------------------------------- */

os_bq_t *osb_bq_create(void)
{
    os_bq_t *q = (os_bq_t *)osb_alloc(sizeof(os_bq_t));
    /* The A20 allocator returns uninitialised storage (unlike the host
     * calloc); a non-zero lock word would wedge the first acquire. */
    if (q)
        os_memzero(q, sizeof(*q));
    return q;
}

void osb_bq_destroy(os_bq_t *q)
{
    if (q)
        osb_free(q);
}

static void bq_lock(struct os_bq *q)
{
    for (;;) {
        int exchanged = 0;
        (void)osn_atomic32(&q->lock, OSN_ATOMIC_CAS, 0, 1, &exchanged);
        if (exchanged)
            return;
        (void)osn_futex_wait(&q->lock, 1, A20_FUTEX_FOREVER);
    }
}

static void bq_unlock(struct os_bq *q)
{
    (void)osn_atomic32(&q->lock, OSN_ATOMIC_STORE, 0, 0, NULL);
    (void)osn_futex_wake(&q->lock, 1);
}

void osb_bq_lock(os_bq_t *q) { bq_lock(q); }
void osb_bq_unlock(os_bq_t *q) { bq_unlock(q); }

int osb_bq_wait_until(os_bq_t *q, uint64_t deadline_ns)
{
    uint32_t seq = osn_atomic32(&q->seq, OSN_ATOMIC_LOAD, 0, 0, NULL);
    bq_unlock(q);

    uint64_t now = osb_time_ns();
    uint64_t rel;
    if (deadline_ns == OS_FOREVER_NS)
        rel = A20_FUTEX_FOREVER;
    else
        rel = deadline_ns > now ? deadline_ns - now : 0;

    (void)osn_futex_wait(&q->seq, seq, rel);
    bq_lock(q);

    if (deadline_ns != OS_FOREVER_NS && osb_time_ns() >= deadline_ns)
        return -OS_ETIMEDOUT;
    return 0;
}

void osb_bq_wake(os_bq_t *q, unsigned count)
{
    (void)osn_atomic32(&q->seq, OSN_ATOMIC_STORE, 0, q->seq + 1, NULL);
    (void)osn_futex_wake(&q->seq, count ? count : 1);
}

void osb_bq_wake_all(os_bq_t *q)
{
    (void)osn_atomic32(&q->seq, OSN_ATOMIC_STORE, 0, q->seq + 1, NULL);
    (void)osn_futex_wake(&q->seq, 0x7FFFFFFFU);
}

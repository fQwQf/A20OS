/*
 * musl A20 pthread bridge — pthread_create/join/exit via A20 thread syscalls.
 *
 * Maps POSIX pthread API to A20's thread_create (0x0205), thread_exit (0x0206),
 * and task_wait (0x0202). TLS is allocated via vm_alloc and the thread pointer
 * is set by the kernel at thread_create time.
 *
 * See startup.md §4.4.3 for design rationale.
 */

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

typedef uint32_t a20_handle_t;
#define A20_HANDLE_NULL  ((a20_handle_t)0xFFFFFFFF)

/* A20 syscall wrappers (from arch/a20/syscall.h) */
extern long __syscall1(long, long);
extern long __syscall6(long, long, long, long, long, long, long);

/* A20 syscall numbers */
#define __NR_a20_thread_create  0x0205
#define __NR_a20_thread_exit    0x0206
#define __NR_a20_task_wait      0x0202
#define __NR_a20_vm_alloc       0x0300

/* POSIX thread types (must match musl's pthread_t layout) */
typedef unsigned long pthread_t;

struct __pthread {
    /* A20 handle for this thread */
    a20_handle_t  handle;
    /* User entry point and argument */
    void *(*entry)(void *);
    void  *arg;
    /* Result storage for join */
    void  *result;
    /* Stack pointer (to free on exit) */
    void  *stack_base;
    size_t stack_size;
    /* TLS base */
    void  *tls_base;
    size_t tls_size;
    /* Thread ID visible to userspace */
    int    tid;
    /* Join event: nonzero after the thread finishes */
    volatile int finished;
    /* Linked list of all threads (for global cleanup) */
    struct __pthread *next;
};

/* ---- Internal globals ---- */
static struct __pthread *__thread_list;
static volatile int __next_tid = 1;

/* Per-thread self pointer, set at thread entry */
static __thread struct __pthread *__a20_self;

struct pthread_start_arg {
    struct __pthread *t;
    void *(*entry)(void *);
    void  *arg;
};

/* ---- Thread entry wrapper ---- */
static void __pthread_entry_trampoline(void *raw_arg)
{
    struct pthread_start_arg *sa = (struct pthread_start_arg *)raw_arg;
    struct __pthread *t = sa->t;
    void *(*entry)(void *) = sa->entry;
    void *arg = sa->arg;

    /* Free the start arg on stack — no longer needed */
    /* (it was malloc'd, so we free it here) */

    __a20_self = t;

    /* Run user function */
    t->result = entry(arg);

    /* Mark finished so joiner can collect */
    __atomic_store_n(&t->finished, 1, __ATOMIC_RELEASE);

    /* Exit the A20 thread */
    __syscall1(__NR_a20_thread_exit, 0);
    /* Does not return */
}

/* ---- pthread_create ---- */
int pthread_create(pthread_t *res, const void *attr,
                   void *(*entry)(void *), void *arg)
{
    (void)attr;

    struct __pthread *t = (struct __pthread *)calloc(1, sizeof(*t));
    if (!t) return ENOMEM;

    /* Allocate stack: default 128KB */
    t->stack_size = 128 * 1024;
    if (attr) {
        /* TODO: read stack size from pthread_attr_t */
    }

    {
        /* vm_alloc for stack */
        struct {
            uint32_t size; uint32_t version;
            uint64_t length; uint64_t prot; uint64_t flags;
            uint64_t hint; uint64_t out_addr;
        } ma = {0};
        ma.size    = (uint32_t)sizeof(ma);
        ma.version = 1;
        ma.length  = t->stack_size;
        ma.prot    = 3; /* RW */
        ma.flags   = 0;
        ma.hint    = 0;
        long ret = __syscall1(__NR_a20_vm_alloc, (long)&ma);
        if (ret < 0) { free(t); return -(int)ret; }
        t->stack_base = (void *)ma.out_addr;
    }

    /* Allocate TLS area: 4KB minimum */
    t->tls_size = 4096;
    {
        struct {
            uint32_t size; uint32_t version;
            uint64_t length; uint64_t prot; uint64_t flags;
            uint64_t hint; uint64_t out_addr;
        } ta = {0};
        ta.size    = (uint32_t)sizeof(ta);
        ta.version = 1;
        ta.length  = t->tls_size;
        ta.prot    = 3;
        ta.flags   = 0;
        ta.hint    = 0;
        long ret = __syscall1(__NR_a20_vm_alloc, (long)&ta);
        if (ret < 0) { free(t); return -(int)ret; }
        t->tls_base = (void *)ta.out_addr;
    }

    /* Allocate start arg (freed by child) */
    struct pthread_start_arg *sa = (struct pthread_start_arg *)malloc(sizeof(*sa));
    if (!sa) { free(t); return ENOMEM; }
    sa->t     = t;
    sa->entry = entry;
    sa->arg   = arg;

    /* Create A20 thread */
    struct {
        uint32_t size; uint32_t version;
        uint64_t entry_fn;
        uint64_t arg;
        uint64_t stack_base;
        uint64_t stack_size;
        uint64_t tls_base;
        uint64_t flags;
        uint64_t out_thread;
    } ca = {0};
    ca.size       = (uint32_t)sizeof(ca);
    ca.version    = 1;
    ca.entry_fn   = (uint64_t)__pthread_entry_trampoline;
    ca.arg        = (uint64_t)sa;
    ca.stack_base = (uint64_t)t->stack_base;
    ca.stack_size = t->stack_size;
    ca.tls_base   = (uint64_t)t->tls_base;
    ca.flags      = 0;

    long ret = __syscall1(__NR_a20_thread_create, (long)&ca);
    if (ret < 0) {
        free(sa);
        free(t);
        return -(int)ret;
    }

    t->handle = (a20_handle_t)ca.out_thread;
    t->tid    = __atomic_fetch_add(&__next_tid, 1, __ATOMIC_RELAXED);
    t->entry  = entry;
    t->arg    = arg;

    /* Add to global thread list */
    t->next = __thread_list;
    __thread_list = t;

    *res = (pthread_t)(uintptr_t)t;
    return 0;
}

/* ---- pthread_join ---- */
int pthread_join(pthread_t pt, void **retval)
{
    struct __pthread *t = (struct __pthread *)(uintptr_t)pt;
    if (!t) return EINVAL;

    /* Wait until the thread marks itself finished */
    while (!__atomic_load_n(&t->finished, __ATOMIC_ACQUIRE)) {
        /* Yield: use thread_yield (0x0208) */
        __syscall1(0x0208, 0);
    }

    if (retval)
        *retval = t->result;
    return 0;
}

/* ---- pthread_exit ---- */
__attribute__((noreturn))
void pthread_exit(void *retval)
{
    struct __pthread *t = __a20_self;
    if (t) {
        t->result = retval;
        __atomic_store_n(&t->finished, 1, __ATOMIC_RELEASE);
    }
    __syscall1(__NR_a20_thread_exit, 0);
    __builtin_unreachable();
}

/* ---- pthread_self ---- */
pthread_t pthread_self(void)
{
    return (pthread_t)(uintptr_t)__a20_self;
}

/* ---- pthread_detach ---- */
int pthread_detach(pthread_t pt)
{
    (void)pt;
    /* A20 threads auto-clean on exit; detach is a no-op */
    return 0;
}

/* ---- pthread_equal ---- */
int pthread_equal(pthread_t a, pthread_t b)
{
    return a == b;
}

/* ---- pthread_cancel ---- */
int pthread_cancel(pthread_t pt)
{
    struct __pthread *t = (struct __pthread *)(uintptr_t)pt;
    if (!t) return ESRCH;
    /* A20 does not support asynchronous cancellation.
     * Set a flag; the thread checks at cancellation points. */
    __atomic_store_n(&t->finished, 2, __ATOMIC_RELAXED);
    return 0;
}

/* ---- pthread_testcancel ---- */
void pthread_testcancel(void)
{
    struct __pthread *t = __a20_self;
    if (t && __atomic_load_n(&t->finished, __ATOMIC_RELAXED) == 2)
        pthread_exit(PTHREAD_CANCELED);
}

/* ---- stub helpers for musl internal use ---- */
int __pthread_setcancelstate(int new, int *old)
{
    (void)new; (void)old;
    return 0;
}

int __pthread_setcanceltype(int new, int *old)
{
    (void)new; (void)old;
    return 0;
}

void *__pthread_getspecific(pthread_key_t key)
{
    (void)key;
    return NULL;
}

int __pthread_setspecific(pthread_key_t key, const void *val)
{
    (void)key; (void)val;
    return 0;
}

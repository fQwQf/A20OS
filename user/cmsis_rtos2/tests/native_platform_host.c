/*
 * Host implementation of the Native backend platform calls.
 *
 * It exists so the backend that runs on the MCU can be exercised on a normal
 * Linux process: threads become pthreads, futexes become the kernel futex,
 * and every platform call is counted.  The counts are printed at exit so a
 * test can assert that the personality only used the syscalls its manifest
 * declares.
 */
#define _GNU_SOURCE
#include "native_platform.h"

#include <errno.h>
#include <linux/futex.h>
#include <pthread.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

static unsigned g_calls[OSN_CALL_COUNT];

static const char *const g_call_names[OSN_CALL_COUNT] = {
    "clock_get", "vm_alloc", "thread_create", "thread_exit",
    "thread_sleep", "thread_yield", "task_kill",
    "task_set_sched", "handle_close", "futex_wait", "futex_wake",
    "atomic32",
};

struct host_thread {
    pthread_t tid;
};

struct host_start {
    void (*entry)(void *);
    void *arg;
};

static void *host_trampoline(void *p)
{
    struct host_start s = *(struct host_start *)p;
    free(p);
    s.entry(s.arg);
    return NULL;
}

static void dump_usage(void)
{
    printf("osn-used:");
    int first = 1;
    for (int i = 0; i < OSN_CALL_COUNT; i++) {
        if (!g_calls[i])
            continue;
        printf("%s%s", first ? "" : ",", g_call_names[i]);
        first = 0;
    }
    printf("\n");
    printf("osn-counts:");
    for (int i = 0; i < OSN_CALL_COUNT; i++)
        printf(" %s=%u", g_call_names[i], g_calls[i]);
    printf("\n");
    fflush(stdout);
}

void osn_platform_init(void)
{
    static int once;
    if (!once) {
        once = 1;
        atexit(dump_usage);
    }
}

size_t osn_stack_grow(size_t requested)
{
    return requested < (size_t)PTHREAD_STACK_MIN
               ? (size_t)PTHREAD_STACK_MIN : requested;
}

int osn_clock_get_ns(uint64_t *out_ns)
{
    g_calls[OSN_CLOCK_GET]++;
    struct timespec ts;
    if (clock_gettime(CLOCK_MONOTONIC, &ts) != 0)
        return -1;
    *out_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
    return 0;
}

int osn_vm_alloc(size_t size, void **out_ptr)
{
    g_calls[OSN_VM_ALLOC]++;
    void *p = calloc(1, size);
    if (!p)
        return -1;
    *out_ptr = p;
    return 0;
}

void osn_vm_free(void *ptr) { free(ptr); }

int osn_thread_create(void (*entry)(void *), void *arg, void *stack,
                      size_t stack_size, void **out_handle)
{
    g_calls[OSN_THREAD_CREATE]++;
    struct host_thread *h = (struct host_thread *)calloc(1, sizeof(*h));
    struct host_start *s = (struct host_start *)calloc(1, sizeof(*s));
    if (!h || !s) {
        free(h);
        free(s);
        return -1;
    }
    s->entry = entry;
    s->arg = arg;

    pthread_attr_t attr;
    pthread_attr_init(&attr);
    if (stack && stack_size >= (size_t)PTHREAD_STACK_MIN)
        (void)pthread_attr_setstack(&attr, stack, stack_size);
    int rc = pthread_create(&h->tid, &attr, host_trampoline, s);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        free(h);
        free(s);
        return -1;
    }
    *out_handle = h;
    return 0;
}

void osn_thread_exit(void)
{
    g_calls[OSN_THREAD_EXIT]++;
    pthread_exit(NULL);
}

void osn_thread_reap(void *handle)
{
    struct host_thread *h = (struct host_thread *)handle;
    if (h)
        (void)pthread_join(h->tid, NULL);
}

int osn_thread_sleep_ns(uint64_t ns)
{
    g_calls[OSN_THREAD_SLEEP]++;
    struct timespec ts = {
        .tv_sec = (time_t)(ns / 1000000000ULL),
        .tv_nsec = (long)(ns % 1000000000ULL),
    };
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR)
            return -1;
    }
    return 0;
}

void osn_thread_yield(void)
{
    g_calls[OSN_THREAD_YIELD]++;
    sched_yield();
}

int osn_task_kill(void *handle)
{
    g_calls[OSN_TASK_KILL]++;
    struct host_thread *h = (struct host_thread *)handle;
    return pthread_cancel(h->tid) == 0 ? 0 : -1;
}

int osn_task_set_sched(void *handle, uint32_t fields, int32_t priority,
                       uint32_t affinity)
{
    g_calls[OSN_TASK_SET_SCHED]++;
    (void)handle;
    (void)fields;
    (void)priority;
    (void)affinity;
    return 0;
}

int osn_handle_close(void *handle)
{
    g_calls[OSN_HANDLE_CLOSE]++;
    struct host_thread *h = (struct host_thread *)handle;
    if (!h)
        return 0;
    (void)pthread_detach(h->tid);
    free(h);
    return 0;
}

int osn_futex_wait(uint32_t *addr, uint32_t expected, uint64_t timeout_ns)
{
    g_calls[OSN_FUTEX_WAIT]++;
    struct timespec ts;
    struct timespec *tsp = NULL;
    if (timeout_ns != UINT64_MAX) {
        ts.tv_sec = (time_t)(timeout_ns / 1000000000ULL);
        ts.tv_nsec = (long)(timeout_ns % 1000000000ULL);
        tsp = &ts;
    }
    long rc = syscall(SYS_futex, addr, FUTEX_WAIT, expected, tsp, NULL, 0);
    if (rc == 0)
        return 0;
    if (errno == EAGAIN)   /* value changed before sleeping: a wake */
        return 0;
    return -1;
}

int osn_futex_wake(uint32_t *addr, uint32_t count)
{
    g_calls[OSN_FUTEX_WAKE]++;
    (void)syscall(SYS_futex, addr, FUTEX_WAKE, count, NULL, NULL, 0);
    return 0;
}

uint32_t osn_atomic32(uint32_t *addr, uint32_t op, uint32_t expected,
                      uint32_t desired, int *exchanged)
{
    g_calls[OSN_ATOMIC32]++;
    switch (op) {
    case OSN_ATOMIC_LOAD:
        return __atomic_load_n(addr, __ATOMIC_SEQ_CST);
    case OSN_ATOMIC_STORE:
        return __atomic_exchange_n(addr, desired, __ATOMIC_SEQ_CST);
    case OSN_ATOMIC_XCHG:
        return __atomic_exchange_n(addr, desired, __ATOMIC_SEQ_CST);
    case OSN_ATOMIC_CAS: {
        uint32_t exp = expected;
        int ok = __atomic_compare_exchange_n(addr, &exp, desired, 0,
                                             __ATOMIC_SEQ_CST,
                                             __ATOMIC_SEQ_CST);
        if (exchanged)
            *exchanged = ok;
        return ok ? expected : exp;
    }
    default:
        return 0;
    }
}

const char *osn_call_name(int call)
{
    if (call < 0 || call >= OSN_CALL_COUNT)
        return "?";
    return g_call_names[call];
}

unsigned osn_call_count(int call)
{
    if (call < 0 || call >= OSN_CALL_COUNT)
        return 0;
    return g_calls[call];
}

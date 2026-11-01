/*
 * CMSIS-RTOS2 backend for A20OS builds that expose the Linux ABI.
 *
 * Everything the personality needs already exists there: pthreads, futex
 * backed mutexes and condition variables, and CLOCK_MONOTONIC.  This backend
 * is also what the host unit test exercises, so the semantics of the core are
 * validated on a normal Linux process before a target is involved.
 */
#define _GNU_SOURCE
#include "os_impl.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#define OS_POSIX_TICK_FREQ 1000U

struct os_bq {
    pthread_mutex_t m;
    pthread_cond_t  c;
};

static pthread_key_t  g_self_key;
static int            g_self_key_ready;
static pthread_once_t g_once = PTHREAD_ONCE_INIT;
static pthread_mutex_t g_kernel_m;
static int             g_kernel_depth;
static pthread_mutex_t g_suspend_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_suspend_c = PTHREAD_COND_INITIALIZER;

static uint64_t now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
}

static void suspend_handler(int sig)
{
    (void)sig;
    osThreadId_t self = (osThreadId_t)pthread_getspecific(g_self_key);
    if (!self || !self->suspend_req)
        return;
    pthread_mutex_lock(&g_suspend_m);
    while (self->suspend_req)
        pthread_cond_wait(&g_suspend_c, &g_suspend_m);
    pthread_mutex_unlock(&g_suspend_m);
}

static void posix_init_once(void)
{
    pthread_key_create(&g_self_key, NULL);
    g_self_key_ready = 1;

    pthread_mutexattr_t ma;
    pthread_mutexattr_init(&ma);
    pthread_mutexattr_settype(&ma, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&g_kernel_m, &ma);
    pthread_mutexattr_destroy(&ma);

    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = suspend_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGUSR1, &sa, NULL);
}

int osb_init(void)
{
    pthread_once(&g_once, posix_init_once);
    return 0;
}

void *osb_alloc(size_t size) { return malloc(size); }
void  osb_free(void *ptr)    { free(ptr); }

uint64_t osb_time_ns(void) { return now_ns(); }
uint32_t osb_tick_freq(void) { return OS_POSIX_TICK_FREQ; }

/* ------------------------------------------------------------------------- */
/* Threads                                                                   */
/* ------------------------------------------------------------------------- */

static void *posix_entry(void *arg)
{
    osThreadId_t t = (osThreadId_t)arg;
    pthread_setspecific(g_self_key, t);
    os_thread_entry(t);
    return NULL;
}

int osb_thread_start(osThreadId_t t)
{
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_JOINABLE);
    if (t->stack_size >= PTHREAD_STACK_MIN)
        pthread_attr_setstacksize(&attr, t->stack_size);
    if (t->stack_mem)
        (void)pthread_attr_setstack(&attr, t->stack_mem, t->stack_size);

    pthread_t tid;
    int rc = pthread_create(&tid, &attr, posix_entry, t);
    pthread_attr_destroy(&attr);
    if (rc != 0)
        return -rc;
    t->bhandle = (void *)(uintptr_t)tid;
    return 0;
}

void osb_thread_exit(void) { pthread_exit(NULL); }

int osb_thread_join(osThreadId_t t)
{
    if (!t->bhandle)
        return -EINVAL;
    int rc = pthread_join((pthread_t)(uintptr_t)t->bhandle, NULL);
    t->bhandle = NULL;
    return rc == 0 ? 0 : -rc;
}

int osb_thread_detach(osThreadId_t t)
{
    if (!t->bhandle)
        return -EINVAL;
    int rc = pthread_detach((pthread_t)(uintptr_t)t->bhandle);
    t->bhandle = NULL;
    return rc == 0 ? 0 : -rc;
}

void osb_thread_yield(void) { sched_yield(); }

int osb_thread_sleep_ns(uint64_t ns)
{
    struct timespec ts;
    ts.tv_sec = (time_t)(ns / 1000000000ULL);
    ts.tv_nsec = (long)(ns % 1000000000ULL);
    while (nanosleep(&ts, &ts) != 0) {
        if (errno != EINTR)
            return -errno;
    }
    return 0;
}

int osb_thread_suspend(osThreadId_t t)
{
    if (!t->bhandle)
        return -EINVAL;
    if (pthread_kill((pthread_t)(uintptr_t)t->bhandle, SIGUSR1) != 0)
        return -ESRCH;
    return 0;
}

int osb_thread_resume(osThreadId_t t)
{
    (void)t;
    pthread_mutex_lock(&g_suspend_m);
    pthread_cond_broadcast(&g_suspend_c);
    pthread_mutex_unlock(&g_suspend_m);
    return 0;
}

int osb_thread_terminate(osThreadId_t t)
{
    if (!t->bhandle)
        return -EINVAL;
    return pthread_cancel((pthread_t)(uintptr_t)t->bhandle) == 0 ? 0 : -ESRCH;
}

osThreadId_t osb_thread_self(void)
{
    if (!g_self_key_ready)
        return NULL;
    return (osThreadId_t)pthread_getspecific(g_self_key);
}

void osb_thread_set_self(osThreadId_t t)
{
    if (g_self_key_ready)
        pthread_setspecific(g_self_key, t);
}

int osb_thread_set_priority(osThreadId_t t, osPriority_t prio)
{
    if (!t->bhandle)
        return 0;
    struct sched_param sp;
    memset(&sp, 0, sizeof(sp));
    sp.sched_priority = 0;
    (void)pthread_setschedparam((pthread_t)(uintptr_t)t->bhandle,
                                SCHED_OTHER, &sp);
    (void)prio;
    return 0;
}

osPriority_t osb_thread_get_priority(osThreadId_t t)
{
    (void)t;
    return osPriorityNone;
}

int osb_thread_set_affinity(osThreadId_t t, uint32_t cpu_set)
{
    if (!t->bhandle)
        return -EINVAL;
    cpu_set_t set;
    CPU_ZERO(&set);
    for (unsigned i = 0; i < 32; i++)
        if (cpu_set & (1U << i))
            CPU_SET(i, &set);
    return pthread_setaffinity_np((pthread_t)(uintptr_t)t->bhandle,
                                  sizeof(set), &set) == 0 ? 0 : -EINVAL;
}

uint32_t osb_thread_get_affinity(osThreadId_t t)
{
    if (!t->bhandle)
        return 0;
    cpu_set_t set;
    CPU_ZERO(&set);
    if (pthread_getaffinity_np((pthread_t)(uintptr_t)t->bhandle,
                               sizeof(set), &set) != 0)
        return 0;
    uint32_t mask = 0;
    for (unsigned i = 0; i < 32; i++)
        if (CPU_ISSET(i, &set))
            mask |= 1U << i;
    return mask;
}

uint32_t osb_thread_stack_space(osThreadId_t t)
{
    (void)t;
    return 0;   /* pthread does not expose a portable used-stack figure */
}

/* ------------------------------------------------------------------------- */
/* Kernel lock                                                               */
/* ------------------------------------------------------------------------- */

int osb_kernel_lock(void)
{
    pthread_mutex_lock(&g_kernel_m);
    int prev = g_kernel_depth;
    g_kernel_depth++;
    return prev;
}

int osb_kernel_unlock(void)
{
    if (g_kernel_depth > 0)
        g_kernel_depth--;
    int now = g_kernel_depth;
    pthread_mutex_unlock(&g_kernel_m);
    return now;
}

int osb_kernel_restore(int depth)
{
    int prev = g_kernel_depth;
    while (g_kernel_depth > depth) {
        g_kernel_depth--;
        pthread_mutex_unlock(&g_kernel_m);
    }
    while (g_kernel_depth < depth) {
        pthread_mutex_lock(&g_kernel_m);
        g_kernel_depth++;
    }
    return prev;
}

/* ------------------------------------------------------------------------- */
/* Blocking queue                                                            */
/* ------------------------------------------------------------------------- */

os_bq_t *osb_bq_create(void)
{
    struct os_bq *q = (struct os_bq *)calloc(1, sizeof(*q));
    if (!q)
        return NULL;
    pthread_mutex_init(&q->m, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, CLOCK_MONOTONIC);
    pthread_cond_init(&q->c, &ca);
    pthread_condattr_destroy(&ca);
    return q;
}

void osb_bq_destroy(os_bq_t *q)
{
    if (!q)
        return;
    pthread_cond_destroy(&q->c);
    pthread_mutex_destroy(&q->m);
    free(q);
}

void osb_bq_lock(os_bq_t *q) { pthread_mutex_lock(&q->m); }
void osb_bq_unlock(os_bq_t *q) { pthread_mutex_unlock(&q->m); }

int osb_bq_wait_until(os_bq_t *q, uint64_t deadline_ns)
{
    if (deadline_ns == OS_FOREVER_NS) {
        int rc = pthread_cond_wait(&q->c, &q->m);
        return rc == 0 ? 0 : -rc;
    }
    struct timespec ts;
    ts.tv_sec = (time_t)(deadline_ns / 1000000000ULL);
    ts.tv_nsec = (long)(deadline_ns % 1000000000ULL);
    int rc = pthread_cond_timedwait(&q->c, &q->m, &ts);
    return rc == ETIMEDOUT ? -ETIMEDOUT : (rc == 0 ? 0 : -rc);
}

void osb_bq_wake(os_bq_t *q, unsigned count)
{
    while (count--)
        pthread_cond_signal(&q->c);
}

void osb_bq_wake_all(os_bq_t *q)
{
    pthread_cond_broadcast(&q->c);
}

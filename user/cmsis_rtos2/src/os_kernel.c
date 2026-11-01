/*
 * CMSIS-RTOS2 kernel object: lifecycle, clock, scheduler lock and delays.
 *
 * The "kernel" here is the A20OS scheduler seen through the backend.  The
 * library does not implement scheduling; it only tracks the CMSIS-visible
 * state, converts ticks to the backend clock and provides the objects that
 * CMSIS applications expect.
 */
#include "os_impl.h"


static osKernelState_t g_state = osKernelInactive;
static int32_t         g_lock_depth;
static os_bq_t        *g_reg_q;          /* protects g_threads and timers */
static struct os_thread_cb g_main_thread;
static struct os_thread_cb *g_threads;
static uint32_t        g_thread_count;

int os_core_init(void)
{
    if (g_state != osKernelInactive)
        return 0;

    if (osb_init() != 0)
        return -1;

    g_reg_q = osb_bq_create();
    if (!g_reg_q)
        return -1;
#if OS_CFG_TIMER
    if (os_timer_core_init() != 0)
        return -1;
#endif

    os_memzero(&g_main_thread, sizeof(g_main_thread));
    g_main_thread.obj.magic = OS_MAGIC;
    g_main_thread.obj.kind = OS_KIND_THREAD;
    g_main_thread.obj.static_cb = 1;
    g_main_thread.obj.q = osb_bq_create();
    if (!g_main_thread.obj.q)
        return -1;
    os_name_store(g_main_thread.obj.name, "main");
    g_main_thread.priority = osPriorityNormal;
    g_main_thread.state = osThreadRunning;
    g_main_thread.stack_size = 0;

    os_thread_registry_add(&g_main_thread);
    osb_thread_set_self(&g_main_thread);
    g_state = osKernelReady;
    return 0;
}

osKernelState_t osKernelGetState(void)
{
    if (g_state == osKernelRunning && g_lock_depth > 0)
        return osKernelLocked;
    return g_state;
}

osStatus_t osKernelInitialize(void)
{
    if (os_core_init() != 0)
        return osError;
    return osOK;
}

osStatus_t osKernelStart(void)
{
    if (g_state == osKernelInactive)
        return osError;
    if (g_state == osKernelRunning)
        return osOK;

#if OS_CFG_TIMER
    os_timer_service_start();
#endif
    g_state = osKernelRunning;
    return osOK;
}

uint32_t osKernelGetVersion(void)
{
    return OS_API_VERSION;
}

osStatus_t osKernelGetInfo(osVersion_t *version, char *id_buf, uint32_t id_size)
{
    static const char id[] = "A20OS CMSIS-RTOS2";

    if (version) {
        version->api = OS_API_VERSION;
        version->kernel = OS_API_VERSION;
    }
    if (id_buf && id_size) {
        uint32_t n = sizeof(id);
        if (n > id_size)
            n = id_size;
        os_memcopy(id_buf, id, n);
        id_buf[n - 1] = '\0';
    }
    return osOK;
}

/* ------------------------------------------------------------------------- */
/* Clock                                                                     */
/* ------------------------------------------------------------------------- */

uint64_t os_ticks_to_ns(uint64_t ticks)
{
    uint32_t freq = osb_tick_freq();
    if (!freq)
        return 0;
    return ticks * (1000000000ULL / freq);
}

uint64_t os_ns_to_ticks(uint64_t ns)
{
    uint32_t freq = osb_tick_freq();
    if (!freq)
        return 0;
    return ns / (1000000000ULL / freq);
}

uint32_t osKernelGetTickCount(void)
{
    return (uint32_t)os_ns_to_ticks(osb_time_ns());
}

uint32_t osKernelGetTickFreq(void)
{
    return osb_tick_freq();
}

uint32_t osKernelGetSysTimerCount(void)
{
    return (uint32_t)osb_time_ns();
}

uint32_t osKernelGetSysTimerFreq(void)
{
    return 1000000000U;
}

uint64_t os_deadline_ns(uint32_t timeout_ticks)
{
    if (timeout_ticks == osWaitForever)
        return OS_FOREVER_NS;
    return osb_time_ns() + os_ticks_to_ns(timeout_ticks);
}

/* ------------------------------------------------------------------------- */
/* Scheduler lock                                                            */
/* ------------------------------------------------------------------------- */

int32_t osKernelLock(void)
{
    if (g_state != osKernelRunning)
        return (int32_t)osErrorISR;
    int prev = osb_kernel_lock();
    g_lock_depth++;
    return prev > 0 ? 1 : 0;
}

int32_t osKernelUnlock(void)
{
    if (g_state != osKernelRunning)
        return (int32_t)osErrorISR;
    if (g_lock_depth <= 0)
        return (int32_t)osError;
    g_lock_depth--;
    int now = osb_kernel_unlock();
    return now > 0 ? 1 : 0;
}

int32_t osKernelRestoreLock(int32_t lock)
{
    if (g_state != osKernelRunning)
        return (int32_t)osErrorISR;
    if (lock < 0)
        return (int32_t)osErrorParameter;
    int prev = osb_kernel_restore(lock);
    g_lock_depth = lock;
    return prev;
}

uint32_t osKernelSuspend(void)
{
    if (g_state != osKernelRunning)
        return 0;
    g_state = osKernelSuspended;
    return 0;   /* no tickless support: the timer service keeps running */
}

void osKernelResume(uint32_t sleep_ticks)
{
    (void)sleep_ticks;
    if (g_state == osKernelSuspended)
        g_state = osKernelRunning;
}

/* ------------------------------------------------------------------------- */
/* Delay                                                                     */
/* ------------------------------------------------------------------------- */

void osDelay(uint32_t ticks)
{
    if (ticks == 0) {
        osb_thread_yield();
        return;
    }
    osThreadId_t self = os_current_thread();
    if (self && self->obj.magic == OS_MAGIC)
        self->state = osThreadBlocked;
    (void)osb_thread_sleep_ns(os_ticks_to_ns(ticks));
    if (self && self->obj.magic == OS_MAGIC)
        self->state = osThreadRunning;
}

osStatus_t osDelayUntil(uint32_t ticks)
{
    uint32_t now = osKernelGetTickCount();
    /* A deadline already in the past is not a 49-day sleep. */
    if ((int32_t)(ticks - now) <= 0)
        return osOK;
    osDelay(ticks - now);
    return osOK;
}

/* ------------------------------------------------------------------------- */
/* Shared helpers                                                            */
/* ------------------------------------------------------------------------- */

void os_name_store(char dst[OS_CFG_NAME_MAX], const char *src)
{
    if (!src) {
        dst[0] = '\0';
        return;
    }
    size_t n = os_strlen(src);
    if (n >= OS_CFG_NAME_MAX)
        n = OS_CFG_NAME_MAX - 1;
    os_memcopy(dst, src, n);
    dst[n] = '\0';
}

int os_status_to_errno(osStatus_t st)
{
    switch (st) {
    case osOK:              return 0;
    case osErrorTimeout:    return -OS_ETIMEDOUT;
    case osErrorResource:   return -OS_EAGAIN;
    case osErrorParameter:  return -OS_EINVAL;
    case osErrorNoMemory:   return -OS_ENOMEM;
    case osErrorISR:        return -OS_EPERM;
    default:                return -OS_EIO;
    }
}

osThreadId_t os_current_thread(void)
{
    osThreadId_t t = osb_thread_self();
    if (t && t->obj.magic == OS_MAGIC)
        return t;
    return &g_main_thread;
}

int os_thread_registry_add(osThreadId_t t)
{
    if (!g_reg_q)
        return -1;
    osb_bq_lock(g_reg_q);
    t->next = g_threads;
    g_threads = t;
    g_thread_count++;
    osb_bq_unlock(g_reg_q);
    return 0;
}

void os_thread_registry_remove(osThreadId_t t)
{
    if (!g_reg_q)
        return;
    osb_bq_lock(g_reg_q);
    struct os_thread_cb **pp = &g_threads;
    while (*pp) {
        if (*pp == t) {
            *pp = t->next;
            g_thread_count--;
            break;
        }
        pp = &(*pp)->next;
    }
    osb_bq_unlock(g_reg_q);
}

void os_reg_lock(void)
{
    if (g_reg_q)
        osb_bq_lock(g_reg_q);
}

void os_reg_unlock(void)
{
    if (g_reg_q)
        osb_bq_unlock(g_reg_q);
}

uint32_t os_thread_registry_count(void)
{
    uint32_t n;
    osb_bq_lock(g_reg_q);
    n = g_thread_count;
    osb_bq_unlock(g_reg_q);
    return n;
}

uint32_t os_thread_registry_enum(osThreadId_t *arr, uint32_t n)
{
    uint32_t i = 0;
    osb_bq_lock(g_reg_q);
    for (struct os_thread_cb *t = g_threads; t && i < n; t = t->next)
        arr[i++] = t;
    osb_bq_unlock(g_reg_q);
    return i;
}

/* ------------------------------------------------------------------------- */
/* Entry trampoline used by every backend                                    */
/* ------------------------------------------------------------------------- */

void os_thread_entry(void *arg)
{
    osThreadId_t self = (osThreadId_t)arg;

    osb_thread_set_self(self);
    self->state = osThreadRunning;
    self->func(self->arg);
    self->state = osThreadTerminated;
    if (self->detached)
        os_thread_release(self);
    osb_thread_exit();
    for (;;) { }   /* a backend must not return here */
}

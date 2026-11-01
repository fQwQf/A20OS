/*
 * CMSIS-RTOS2 software timers.
 *
 * A single service thread owns the timer list.  Callbacks run in that thread,
 * as the CMSIS-RTOS2 specification requires, so a callback may use blocking
 * APIs.  Deadlines are absolute nanoseconds: a periodic timer does not drift
 * by tick rounding.
 *
 * The list and the service wait share one lock (g_timer_q).  "Scan for the
 * next deadline and sleep until it" is therefore one atomic step: a start or
 * stop that lands in between cannot be missed.
 */
#include "os_impl.h"

#if OS_CFG_TIMER

static struct os_timer_cb *g_timers;
static os_bq_t            *g_timer_q;
static volatile int        g_timer_ready;

static int timer_valid(osTimerId_t t)
{
    return t && t->obj.magic == OS_MAGIC && t->obj.kind == OS_KIND_TIMER;
}

int os_timer_core_init(void)
{
    if (!g_timer_q)
        g_timer_q = osb_bq_create();
    return g_timer_q ? 0 : -1;
}

static void os_timer_service(void *arg)
{
    (void)arg;
    for (;;) {
        osb_bq_lock(g_timer_q);
        uint64_t next = OS_FOREVER_NS;
        for (struct os_timer_cb *t = g_timers; t; t = t->next)
            if (t->running && t->next_ns < next)
                next = t->next_ns;
        (void)osb_bq_wait_until(g_timer_q, next);
        osb_bq_unlock(g_timer_q);

        osTimerFunc_t fns[8];
        void         *args[8];
        unsigned      n = 0;
        uint64_t      now = osb_time_ns();

        osb_bq_lock(g_timer_q);
        for (struct os_timer_cb *t = g_timers; t && n < 8; t = t->next) {
            if (!t->running || t->next_ns > now)
                continue;
            fns[n] = t->func;
            args[n] = t->arg;
            n++;
            if (t->type == osTimerPeriodic) {
                uint64_t period = os_ticks_to_ns(t->ticks);
                t->next_ns += period;
                if (t->next_ns <= now)
                    t->next_ns = now + period;
            } else {
                t->running = 0;
            }
        }
        osb_bq_unlock(g_timer_q);

        for (unsigned i = 0; i < n; i++)
            fns[i](args[i]);
    }
}

void os_timer_service_start(void)
{
    if (g_timer_ready)
        return;
    if (os_timer_core_init() != 0)
        return;
    osThreadAttr_t attr = {0};
    attr.name = "cmsis_timer";
    attr.priority = osPriorityAboveNormal;
    attr.stack_size = OS_CFG_TIMER_THREAD_STACK;
    g_timer_ready = osThreadNew(os_timer_service, NULL, &attr) != NULL;
}

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void *argument,
                       const osTimerAttr_t *attr)
{
    if (!func || (type != osTimerOnce && type != osTimerPeriodic))
        return NULL;
    if (os_core_init() != 0)
        return NULL;

    struct os_timer_cb *t;
    int static_cb = 0;
    if (attr && attr->cb_mem) {
        if (attr->cb_size < sizeof(*t))
            return NULL;
        t = (struct os_timer_cb *)attr->cb_mem;
        static_cb = 1;
    } else {
        t = (struct os_timer_cb *)osb_alloc(sizeof(*t));
        if (!t)
            return NULL;
    }
    os_memzero(t, sizeof(*t));
    t->obj.magic = OS_MAGIC;
    t->obj.kind = OS_KIND_TIMER;
    t->obj.static_cb = (uint16_t)static_cb;
    os_name_store(t->obj.name, (attr && attr->name) ? attr->name : "timer");
    t->func = func;
    t->arg = argument;
    t->type = type;
    t->running = 0;
    t->next_ns = OS_FOREVER_NS;

    osb_bq_lock(g_timer_q);
    t->next = g_timers;
    g_timers = t;
    osb_bq_wake_all(g_timer_q);
    osb_bq_unlock(g_timer_q);
    return t;
}

const char *osTimerGetName(osTimerId_t timer_id)
{
    return timer_valid(timer_id) ? timer_id->obj.name : NULL;
}

osStatus_t osTimerStart(osTimerId_t timer_id, uint32_t ticks)
{
    if (!timer_valid(timer_id) || ticks == 0)
        return osErrorParameter;
    osb_bq_lock(g_timer_q);
    timer_id->ticks = ticks;
    timer_id->next_ns = osb_time_ns() + os_ticks_to_ns(ticks);
    timer_id->running = 1;
    osb_bq_wake_all(g_timer_q);
    osb_bq_unlock(g_timer_q);
    return osOK;
}

osStatus_t osTimerStop(osTimerId_t timer_id)
{
    if (!timer_valid(timer_id))
        return osErrorParameter;
    osb_bq_lock(g_timer_q);
    timer_id->running = 0;
    osb_bq_wake_all(g_timer_q);
    osb_bq_unlock(g_timer_q);
    return osOK;
}

uint32_t osTimerIsRunning(osTimerId_t timer_id)
{
    return timer_valid(timer_id) && timer_id->running ? 1U : 0U;
}

osStatus_t osTimerDelete(osTimerId_t timer_id)
{
    if (!timer_valid(timer_id))
        return osErrorParameter;
    osb_bq_lock(g_timer_q);
    struct os_timer_cb **pp = &g_timers;
    while (*pp) {
        if (*pp == timer_id) {
            *pp = timer_id->next;
            break;
        }
        pp = &(*pp)->next;
    }
    osb_bq_wake_all(g_timer_q);
    osb_bq_unlock(g_timer_q);

    timer_id->obj.magic = 0;
    if (!timer_id->obj.static_cb)
        osb_free(timer_id);
    return osOK;
}

#else /* !OS_CFG_TIMER */

int os_timer_core_init(void) { return 0; }
void os_timer_service_start(void) { }
osTimerId_t osTimerNew(osTimerFunc_t f, osTimerType_t t, void *a,
                       const osTimerAttr_t *at)
{ (void)f; (void)t; (void)a; (void)at; return NULL; }
const char *osTimerGetName(osTimerId_t t) { (void)t; return NULL; }
osStatus_t osTimerStart(osTimerId_t t, uint32_t ticks)
{ (void)t; (void)ticks; return osErrorResource; }
osStatus_t osTimerStop(osTimerId_t t) { (void)t; return osErrorResource; }
uint32_t osTimerIsRunning(osTimerId_t t) { (void)t; return 0; }
osStatus_t osTimerDelete(osTimerId_t t) { (void)t; return osErrorResource; }

#endif

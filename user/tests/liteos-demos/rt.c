/*
 * LiteOS textbook experiment 4.5: static priorities and inheritance.
 *
 * 4.1-4.4 prove that the CMSIS-RTOS2 objects work.  This program proves the
 * property that makes them an RTOS rather than a threading library:
 *
 *   - a thread created with a higher osPriority runs before a lower one;
 *   - osThreadSetPriority takes effect on a runnable thread;
 *   - waking several waiters runs them in priority order;
 *   - a mutex owner inherits the priority of a blocked waiter, so a
 *     medium-priority thread cannot delay it (bounded priority inversion).
 *
 * Only the standard API from cmsis_os2.h is used.
 */
#include "cmsis_os2.h"
#include "a20_mem.h"

void demo_log(const char *fmt, ...);
int demo_check(int condition, const char *what);

#define RT_STACK 2048U

static osEventFlagsId_t g_ev;
static osSemaphoreId_t g_ready;

/*
 * Static stacks: the demo heap is a 130 KiB NOMMU arena shared with the
 * kernel, and each 2 KiB request can force a fresh 16 KiB heap region.  The
 * three worker slots are reused by the sequential subtests.
 */
static uint64_t g_stacks[3][RT_STACK / 8] __attribute__((aligned(16)));

static void rt_attr(osThreadAttr_t *attr, const char *name, osPriority_t prio,
                    unsigned slot)
{
    attr->name = name;
    attr->attr_bits = 0;
    attr->cb_mem = NULL;
    attr->cb_size = 0;
    attr->stack_mem = g_stacks[slot];
    attr->stack_size = RT_STACK;
    attr->priority = prio;
}

/* ---------------------------------------------------------------------- */
/* 1. priority applied at creation, and priority-ordered wake             */
/* ---------------------------------------------------------------------- */

static volatile int g_low_ran;
static volatile int g_high_ran;
static volatile int g_high_saw_low;

static void prio_low(void *arg)
{
    (void)arg;
    osSemaphoreRelease(g_ready);
    (void)osEventFlagsWait(g_ev, 1U, osFlagsWaitAny | osFlagsNoClear,
                           osWaitForever);
    g_low_ran = 1;
    /* Do not spin at RT priority: the high thread must get the CPU. */
    for (unsigned i = 0; i < 1000 && !g_high_ran; i++)
        osThreadYield();
}

static void prio_high(void *arg)
{
    (void)arg;
    osSemaphoreRelease(g_ready);
    (void)osEventFlagsWait(g_ev, 1U, osFlagsWaitAny | osFlagsNoClear,
                           osWaitForever);
    g_high_saw_low = g_low_ran;
    g_high_ran = 1;
}

static int test_create_priority(void)
{
    demo_log("[4.5] create-priority");
    int ok = demo_check(g_ev && g_ready, "event flag / semaphore created");
    if (!ok)
        return 0;

    g_low_ran = g_high_ran = g_high_saw_low = 0;
    osThreadAttr_t low_attr, high_attr;
    rt_attr(&low_attr, "prio-low", osPriorityLow, 0);
    rt_attr(&high_attr, "prio-high", osPriorityHigh, 1);
    osThreadId_t low = osThreadNew(prio_low, NULL, &low_attr);
    osThreadId_t high = osThreadNew(prio_high, NULL, &high_attr);
    ok &= demo_check(low && high, "two priority threads created");
    ok &= demo_check(low && high &&
                         osThreadGetPriority(low) == osPriorityLow &&
                         osThreadGetPriority(high) == osPriorityHigh,
                     "creation priorities reached the kernel");
    if (low && high) {
        /* Both are blocked on the event flag now. */
        (void)osSemaphoreAcquire(g_ready, 1000);
        (void)osSemaphoreAcquire(g_ready, 1000);
        (void)osEventFlagsSet(g_ev, 1U);
        osThreadJoin(low);
        osThreadJoin(high);
    }
    ok &= demo_check(g_high_saw_low == 0,
                     "high-priority waiter ran before the low one");
    demo_log("[4.5] create-priority done ok=%d", ok);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* 2. osThreadSetPriority changes a runnable thread                       */
/* ---------------------------------------------------------------------- */

static volatile int g_a_ran;
static volatile int g_b_ran;
static volatile int g_b_saw_a;

static void setprio_a(void *arg)
{
    (void)arg;
    osSemaphoreRelease(g_ready);
    (void)osEventFlagsWait(g_ev, 2U, osFlagsWaitAny | osFlagsNoClear,
                           osWaitForever);
    g_a_ran = 1;
    for (unsigned i = 0; i < 1000 && !g_b_ran; i++)
        osThreadYield();
}

static void setprio_b(void *arg)
{
    (void)arg;
    osSemaphoreRelease(g_ready);
    (void)osEventFlagsWait(g_ev, 2U, osFlagsWaitAny | osFlagsNoClear,
                           osWaitForever);
    g_b_saw_a = g_a_ran;
    g_b_ran = 1;
}

static int test_set_priority(void)
{
    demo_log("[4.5] set-priority");
    g_a_ran = g_b_ran = g_b_saw_a = 0;
    osThreadAttr_t attr_a, attr_b;
    rt_attr(&attr_a, "setprio-a", osPriorityLow, 0);
    rt_attr(&attr_b, "setprio-b", osPriorityLow, 1);
    osThreadId_t a = osThreadNew(setprio_a, NULL, &attr_a);
    osThreadId_t b = osThreadNew(setprio_b, NULL, &attr_b);
    int ok = demo_check(a && b, "equal-priority threads created");
    if (a && b) {
        (void)osSemaphoreAcquire(g_ready, 1000);
        (void)osSemaphoreAcquire(g_ready, 1000);
        ok &= demo_check(osThreadSetPriority(b, osPriorityRealtime) == osOK,
                         "osThreadSetPriority on a blocked thread");
        ok &= demo_check(osThreadGetPriority(b) == osPriorityRealtime,
                         "raised priority reached the kernel");
        (void)osEventFlagsSet(g_ev, 2U);
        osThreadJoin(a);
        osThreadJoin(b);
    }
    ok &= demo_check(g_b_saw_a == 0,
                     "raised thread ran before its equal-priority peer");
    demo_log("[4.5] set-priority done ok=%d", ok);
    return ok;
}


/* ---------------------------------------------------------------------- */
/* 3. osKernelLock defers a higher-priority ready thread                  */
/* ---------------------------------------------------------------------- */

static volatile int g_locked_ran;

static void locked_runner(void *arg)
{
    (void)arg;
    g_locked_ran = 1;
}

static int test_kernel_lock(void)
{
    demo_log("[4.5] kernel-lock");
    g_locked_ran = 0;
    int ok = demo_check(osKernelLock() == 0, "osKernelLock first level");

    osThreadAttr_t attr;
    rt_attr(&attr, "locked-runner", osPriorityHigh, 2);
    osThreadId_t t = osThreadNew(locked_runner, NULL, &attr);
    ok &= demo_check(t != NULL, "high thread created while locked");
    /* The higher-priority thread is READY.  The scheduler lock must keep the
     * creator running until osKernelUnlock, not switch at the syscall exit. */
    ok &= demo_check(g_locked_ran == 0,
                     "higher-priority thread did not run while locked");

    ok &= demo_check(osKernelUnlock() == 0, "osKernelUnlock last level");
    if (t)
        osThreadJoin(t);
    ok &= demo_check(g_locked_ran == 1,
                     "released lock let the higher-priority thread run");
    demo_log("[4.5] kernel-lock done ok=%d", ok);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* 4. mutex priority inheritance                                          */
/* ---------------------------------------------------------------------- */

static osMutexId_t g_mtx;
static volatile unsigned g_hog_steps;
static volatile int g_pi_done;
static volatile int g_pi_clean;

static void pi_owner(void *arg)
{
    (void)arg;
    osMutexAcquire(g_mtx, osWaitForever);
    /* Tell main the mutex is held, then block while still owning it.  The
     * blocked owner is the case priority inheritance exists for. */
    osSemaphoreRelease(g_ready);
    (void)osEventFlagsWait(g_ev, 4U, osFlagsWaitAny | osFlagsNoClear,
                           osWaitForever);
    osMutexRelease(g_mtx);
}

static void pi_hog(void *arg)
{
    (void)arg;
    (void)osEventFlagsWait(g_ev, 4U, osFlagsWaitAny | osFlagsNoClear,
                           osWaitForever);
    /* Bounded: a failing inheritance test must still finish. */
    while (!g_pi_done && g_hog_steps < 2000000U)
        g_hog_steps++;
}

static void pi_high(void *arg)
{
    (void)arg;
    osMutexAcquire(g_mtx, osWaitForever);
    g_pi_clean = (g_hog_steps == 0);
    g_pi_done = 1;
    osMutexRelease(g_mtx);
}

static int test_priority_inheritance(void)
{
    demo_log("[4.5] priority-inheritance");
    g_mtx = osMutexNew(NULL);
    int ok = demo_check(g_mtx != NULL, "mutex created");
    if (!ok)
        return 0;

    g_hog_steps = 0;
    g_pi_done = 0;
    g_pi_clean = 0;
    osThreadAttr_t owner_attr, high_attr, hog_attr;
    rt_attr(&owner_attr, "pi-owner", osPriorityLow, 0);
    rt_attr(&high_attr, "pi-high", osPriorityHigh, 1);
    rt_attr(&hog_attr, "pi-hog", osPriorityBelowNormal, 2);

    osThreadId_t owner = osThreadNew(pi_owner, NULL, &owner_attr);
    ok &= demo_check(owner != NULL, "owner thread created");
    if (!owner)
        return ok;
    (void)osSemaphoreAcquire(g_ready, 1000);   /* owner now holds the mutex */

    osThreadId_t high = osThreadNew(pi_high, NULL, &high_attr);
    ok &= demo_check(high != NULL, "high waiter created");
    osThreadId_t hog = osThreadNew(pi_hog, NULL, &hog_attr);
    ok &= demo_check(hog != NULL, "medium hog created");
    /* The owner is blocked inside the critical section and must carry the
     * waiter's priority before it is woken. */
    ok &= demo_check(osThreadGetPriority(owner) == osPriorityHigh,
                     "owner inherited the waiter priority");

    if (high && hog) {
        /* One event wakes the boosted owner and the medium hog together;
         * priority decides who runs. */
        (void)osEventFlagsSet(g_ev, 4U);
        osThreadJoin(owner);
        osThreadJoin(high);
        osThreadJoin(hog);
    }
    ok &= demo_check(g_pi_clean == 1,
                     "owner inherited the waiter priority (no inversion)");
    ok &= demo_check(g_hog_steps == 0,
                     "medium thread did not run inside the critical section");
    demo_log("[4.5] inheritance done ok=%d steps=%u", ok, (unsigned)g_hog_steps);
    return ok;
}


/* ---------------------------------------------------------------------- */
/* 5. chained inheritance: A waits on B, B waits on C                     */
/* ---------------------------------------------------------------------- */

static osMutexId_t g_chain_m1;
static osMutexId_t g_chain_m2;
static volatile int g_chain_owner_done;
static volatile int g_chain_middle_done;
static volatile int g_chain_high_done;

static void chain_owner(void *arg)
{
    (void)arg;
    osMutexAcquire(g_chain_m1, osWaitForever);
    osSemaphoreRelease(g_ready);
    (void)osEventFlagsWait(g_ev, 8U, osFlagsWaitAny | osFlagsNoClear,
                           osWaitForever);
    osMutexRelease(g_chain_m1);
    g_chain_owner_done = 1;
}

static void chain_middle(void *arg)
{
    (void)arg;
    osMutexAcquire(g_chain_m2, osWaitForever);
    osSemaphoreRelease(g_ready);
    /* Holds M2 while waiting on M1: the link that must inherit A's
     * priority and pass it on to C. */
    osMutexAcquire(g_chain_m1, osWaitForever);
    osMutexRelease(g_chain_m1);
    osMutexRelease(g_chain_m2);
    g_chain_middle_done = 1;
}

static void chain_high(void *arg)
{
    (void)arg;
    osMutexAcquire(g_chain_m2, osWaitForever);
    g_chain_high_done = 1;
    osMutexRelease(g_chain_m2);
}

static int test_chained_inheritance(void)
{
    demo_log("[4.5] chained-priority-inheritance");
    g_chain_m1 = osMutexNew(NULL);
    g_chain_m2 = osMutexNew(NULL);
    int ok = demo_check(g_chain_m1 && g_chain_m2, "chain mutexes");
    if (!ok)
        return 0;

    g_chain_owner_done = 0;
    g_chain_middle_done = 0;
    g_chain_high_done = 0;
    osThreadAttr_t owner_attr, middle_attr, high_attr;
    rt_attr(&owner_attr, "chain-owner", osPriorityLow, 0);
    rt_attr(&middle_attr, "chain-middle", osPriorityBelowNormal, 1);
    rt_attr(&high_attr, "chain-high", osPriorityHigh, 2);

    osThreadId_t owner = osThreadNew(chain_owner, NULL, &owner_attr);
    ok &= demo_check(owner != NULL, "chain owner");
    if (!owner)
        return ok;
    (void)osSemaphoreAcquire(g_ready, 1000);   /* C holds M1 */

    osThreadId_t middle = osThreadNew(chain_middle, NULL, &middle_attr);
    ok &= demo_check(middle != NULL, "chain middle");
    if (!middle)
        return ok;
    (void)osSemaphoreAcquire(g_ready, 1000);   /* B holds M2, waits on M1 */

    osThreadId_t high = osThreadNew(chain_high, NULL, &high_attr);
    ok &= demo_check(high != NULL, "chain high waiter");
    /* A is blocked on M2, B on M1 and C on the event.  A's priority must
     * have reached both B and C: the direct check is the effective kernel
     * priority, not a timing threshold. */
    ok &= demo_check(osThreadGetPriority(owner) == osPriorityHigh,
                     "chained donation reached the mutex owner");
    ok &= demo_check(osThreadGetPriority(middle) == osPriorityHigh,
                     "chained donation reached the middle link");

    if (high) {
        (void)osEventFlagsSet(g_ev, 8U);
        osThreadJoin(owner);
        osThreadJoin(middle);
        osThreadJoin(high);
    }
    ok &= demo_check(g_chain_owner_done && g_chain_middle_done &&
                         g_chain_high_done,
                     "all three chain links completed");
    demo_log("[4.5] chained done ok=%d", ok);
    osMutexDelete(g_chain_m1);
    osMutexDelete(g_chain_m2);
    g_chain_m1 = g_chain_m2 = NULL;
    return ok;
}


/* ---------------------------------------------------------------------- */
/* 6. the initial thread is a valid donation target                       */
/* ---------------------------------------------------------------------- */

static volatile int g_init_done;

static void init_high(void *arg)
{
    (void)arg;
    osMutexAcquire(arg, osWaitForever);
    g_init_done = 1;
    osMutexRelease(arg);
}

static int test_initial_inheritance(void)
{
    demo_log("[4.5] initial-thread-inheritance");
    osMutexId_t m = osMutexNew(NULL);
    int ok = demo_check(m != NULL, "initial mutex");
    if (!ok)
        return 0;

    g_init_done = 0;
    (void)osMutexAcquire(m, osWaitForever);
    osThreadAttr_t attr;
    rt_attr(&attr, "init-high", osPriorityHigh, 0);
    osThreadId_t t = osThreadNew(init_high, m, &attr);
    ok &= demo_check(t != NULL, "initial high waiter");
    /* The high thread is blocked on the mutex main owns; with the installed
     * self handle main must now carry the waiter's priority. */
    ok &= demo_check(osThreadGetPriority(osThreadGetId()) == osPriorityHigh,
                     "initial thread inherited the waiter priority");
    (void)osMutexRelease(m);
    if (t)
        osThreadJoin(t);
    ok &= demo_check(g_init_done == 1, "initial waiter completed");
    demo_log("[4.5] initial done ok=%d", ok);
    osMutexDelete(m);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* 4.6 cross-thread suspend/resume                                        */
/* ---------------------------------------------------------------------- */

static volatile uint32_t g_susp_count;
static volatile int      g_susp_stop;
static uint64_t          g_susp_stack[256] __attribute__((aligned(16)));

static void susp_worker(void *arg)
{
    (void)arg;
    while (!g_susp_stop) {
        g_susp_count++;
        osDelay(1);
    }
}

int experiment_46(void)
{
    demo_log("[4.6] cross-thread suspend/resume");
    g_susp_count = 0;
    g_susp_stop = 0;
    osThreadAttr_t attr = {
        .name = "susp",
        .stack_mem = g_susp_stack,
        .stack_size = sizeof(g_susp_stack),
        .priority = osPriorityLow,
    };
    osThreadId_t t = osThreadNew(susp_worker, NULL, &attr);
    int ok = demo_check(t != NULL, "suspend worker created");
    if (!t)
        return 0;

    for (unsigned i = 0; i < 100 && g_susp_count == 0; i++)
        osDelay(1);
    ok &= demo_check(g_susp_count > 0, "worker made progress");
    ok &= demo_check(osThreadSuspend(t) == osOK, "osThreadSuspend");
    osDelay(5);
    uint32_t frozen = g_susp_count;
    osDelay(50);
    int stayed = g_susp_count == frozen;
    ok &= demo_check(stayed, "worker stopped while suspended");
    ok &= demo_check(osThreadResume(t) == osOK, "osThreadResume");
    osDelay(20);
    int resumed = g_susp_count > frozen;
    ok &= demo_check(resumed, "worker resumed");
    g_susp_stop = 1;
    ok &= demo_check(osThreadJoin(t) == osOK, "suspended worker joined");
    demo_log("[4.6] %s suspend=%d resume=%d",
             ok ? "PASS" : "FAIL", stayed, resumed);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* 4.7 osKernelSuspend / osKernelResume tickless idle                     */
/* ---------------------------------------------------------------------- */

int experiment_47(void)
{
    demo_log("[4.7] osKernelSuspend tickless idle");
    uint32_t before = osKernelGetTickCount();
    uint32_t slept = osKernelSuspend();
    uint32_t after = osKernelGetTickCount();
    osKernelResume(slept);
    int ok = demo_check(slept > 0, "kernel suspend returned sleep ticks");
    ok &= demo_check(after - before >= slept, "tick advanced while suspended");
    ok &= demo_check(osKernelGetState() == osKernelRunning,
                     "kernel resumed");
    demo_log("[4.7] %s slept=%u elapsed=%u", ok ? "PASS" : "FAIL",
             (unsigned)slept, (unsigned)(after - before));
    return ok;
}

/* ---------------------------------------------------------------------- */
/* 4.9 PMP allow-list: an unmapped page is no longer reachable from U-mode */
/* ---------------------------------------------------------------------- */

static volatile uint32_t g_pmp_good_write;
static volatile uint32_t g_pmp_probe_entered;
static volatile uint32_t g_pmp_probe_survived;
static uint64_t          g_pmp_stack[256] __attribute__((aligned(16)));
static volatile uint32_t *g_pmp_good;
static volatile uint32_t *g_pmp_bad;

static void pmp_probe(void *arg)
{
    (void)arg;
    /* First prove the allowed dynamic region works... */
    *g_pmp_good = 0x5a5a5a5aU;
    g_pmp_good_write = 1;
    /* ...then touch the page that vm_unmap removed from the allow-list.  The
     * trap path must terminate this thread with SIGSEGV, not the kernel. */
    g_pmp_probe_entered = 1;
    *g_pmp_bad = 0xdeadbeefU;
    g_pmp_probe_survived = 1;
}

int experiment_49(void)
{
    demo_log("[4.9] PMP allow-list isolation");

    uint64_t good_addr = 0, bad_addr = 0;
    int ok = demo_check(a20_vm_alloc_pages(1, 3, &good_addr) == A20_OK,
                        "alloc allowed page");
    ok &= demo_check(a20_vm_alloc_pages(1, 3, &bad_addr) == A20_OK,
                     "alloc victim page");
    if (!ok) {
        demo_log("[4.9] FAIL allocate");
        return 0;
    }

    g_pmp_good = (volatile uint32_t *)(uintptr_t)good_addr;
    g_pmp_bad = (volatile uint32_t *)(uintptr_t)bad_addr;
    g_pmp_good_write = 0;
    g_pmp_probe_entered = 0;
    g_pmp_probe_survived = 0;

    /* Remove the victim from the flat space while it is still mapped in the
     * kernel heap: only the hardware allow-list can reject the later write. */
    ok &= demo_check(a20_vm_unmap(bad_addr, 4096) == A20_OK,
                     "unmap victim page");

    osThreadAttr_t attr = {
        .name = "pmp-probe",
        .stack_mem = g_pmp_stack,
        .stack_size = sizeof(g_pmp_stack),
        .priority = osPriorityLow,
    };
    osThreadId_t t = osThreadNew(pmp_probe, NULL, &attr);
    ok &= demo_check(t != NULL, "probe thread created");
    if (!t) {
        demo_log("[4.9] FAIL create");
        return 0;
    }

    for (unsigned i = 0; i < 200 && !g_pmp_probe_entered; i++)
        osDelay(1);
    /* Give the faulting store a chance to run even if it is not the next
     * instruction after the flag becomes visible. */
    osDelay(20);

    ok &= demo_check(g_pmp_good_write == 1, "allowed page remains writable");
    ok &= demo_check(g_pmp_probe_entered == 1, "probe reached the bad store");
    ok &= demo_check(g_pmp_probe_survived == 0,
                     "bad store killed the thread, not the kernel");
    /* The thread died with SIGSEGV; detach reaps it without waiting. */
    (void)osThreadDetach(t);
    (void)a20_vm_unmap(good_addr, 4096);
    demo_log("[4.9] %s good=%u entered=%u survived=%u",
             ok ? "PASS" : "FAIL", (unsigned)g_pmp_good_write,
             (unsigned)g_pmp_probe_entered,
             (unsigned)g_pmp_probe_survived);
    return ok;
}

int experiment_45(void)
{
    demo_log("[4.5] static priorities and priority inheritance");
    g_ev = osEventFlagsNew(NULL);
    g_ready = osSemaphoreNew(4, 0, NULL);
    int ok = demo_check(g_ev && g_ready, "4.5 objects");
    int p1 = 0, p2 = 0, p3 = 0, p4 = 0, p5 = 0, p6 = 0;
    if (g_ev && g_ready) {
        p1 = test_create_priority();
        p2 = test_set_priority();
        p3 = test_kernel_lock();
        p4 = test_priority_inheritance();
        p5 = test_chained_inheritance();
        p6 = test_initial_inheritance();
        osEventFlagsDelete(g_ev);
        osSemaphoreDelete(g_ready);
        osMutexDelete(g_mtx);
        ok &= p1 && p2 && p3 && p4 && p5 && p6;
    }
    g_ev = NULL;
    g_ready = NULL;
    g_mtx = NULL;
    demo_log("[4.5] %s create=%d setprio=%d lock=%d inherit=%d chain=%d init=%d",
             ok ? "PASS" : "FAIL", p1, p2, p3, p4, p5, p6);
    return ok;
}

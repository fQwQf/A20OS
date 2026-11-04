/*
 * LiteOS textbook experiments 4.1–4.5, ported to A20OS.
 *
 * The programs use only the standard CMSIS-RTOS2 API (osThread/osSemaphore/
 * osTimer).  They run as ordinary U-mode flat programs under the MCU kernel:
 * every blocking call is a Native ABI syscall handled by the personality
 * backend, not a privileged shortcut inside the kernel.
 *
 * Results are written to a run record shared with the launcher, so the demo
 * itself needs no console API beyond CMSIS.
 */
#include "cmsis_os2.h"
#include "a20_mcu_demo.h"

void demo_runtime_init(a20_mcu_demo_record_t *record);
void demo_log(const char *fmt, ...);
int demo_check(int condition, const char *what);
void demo_finish(int status);
int experiment_44(void);
int experiment_45(void);

#define WORKER_STACK 2048U

/* ---------------------------------------------------------------------- */
/* 4.1 threads                                                            */
/* ---------------------------------------------------------------------- */

static volatile uint32_t g_thread_a, g_thread_b;

static void thread_a(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 5; i++) {
        g_thread_a++;
        demo_log("[4.1] threadA step %u", i + 1);
        osDelay(20);
    }
    demo_log("[4.1] threadA finished");
}

static void thread_b(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 5; i++) {
        g_thread_b++;
        demo_log("[4.1] threadB step %u", i + 1);
        osDelay(30);
    }
    demo_log("[4.1] threadB finished");
}

static int experiment_41(void)
{
    const osThreadAttr_t attr = {
        .name = "worker",
        .priority = osPriorityNormal,
        .stack_size = WORKER_STACK,
    };
    demo_log("[4.1] thread create/join, osDelay");
    g_thread_a = 0;
    g_thread_b = 0;
    osThreadId_t a = osThreadNew(thread_a, NULL, &attr);
    osThreadId_t b = osThreadNew(thread_b, NULL, &attr);
    int ok = demo_check(a && b, "osThreadNew returns an id");
    if (a)
        ok &= demo_check(osThreadJoin(a) == osOK, "join threadA");
    if (b)
        ok &= demo_check(osThreadJoin(b) == osOK, "join threadB");
    ok &= demo_check(g_thread_a == 5 && g_thread_b == 5,
                     "both threads ran five steps");
    demo_log("[4.1] %s steps=%u/%u", ok ? "PASS" : "FAIL",
             (unsigned)g_thread_a, (unsigned)g_thread_b);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* 4.2 semaphore                                                          */
/* ---------------------------------------------------------------------- */

static osSemaphoreId_t g_sem;
static volatile uint32_t g_produced;

static void producer(void *arg)
{
    (void)arg;
    for (unsigned i = 0; i < 5; i++) {
        osDelay(10);
        g_produced++;
        demo_log("[4.2] producer release %u -> %d", i + 1,
                 (int)osSemaphoreRelease(g_sem));
    }
}

static int experiment_42(void)
{
    const osThreadAttr_t attr = {
        .name = "producer",
        .priority = osPriorityNormal,
        .stack_size = WORKER_STACK,
    };
    demo_log("[4.2] counting semaphore producer/consumer");
    g_produced = 0;
    g_sem = osSemaphoreNew(5, 0, NULL);
    demo_log("[4.2] semaphore=%x", (unsigned)(uintptr_t)g_sem);
    int ok = demo_check(g_sem != NULL, "osSemaphoreNew");
    osThreadId_t p = osThreadNew(producer, NULL, &attr);
    ok &= demo_check(p != NULL, "producer thread");

    unsigned taken = 0;
    for (unsigned i = 0; i < 5; i++) {
        osStatus_t st = osSemaphoreAcquire(g_sem, 1000);
        demo_log("[4.2] consumer acquire %u -> %d", i + 1, (int)st);
        if (st == osOK)
            taken++;
    }

    if (p)
        osThreadJoin(p);
    ok &= demo_check(taken == 5, "consumer acquired five tokens");
    ok &= demo_check(g_produced == 5, "producer released five tokens");
    ok &= demo_check(osSemaphoreGetCount(g_sem) == 0,
                     "semaphore count back to zero");
    osSemaphoreDelete(g_sem);
    g_sem = NULL;
    demo_log("[4.2] %s taken=%u produced=%u", ok ? "PASS" : "FAIL",
             taken, (unsigned)g_produced);
    return ok;
}

/* ---------------------------------------------------------------------- */
/* 4.3 software timers                                                    */
/* ---------------------------------------------------------------------- */

static volatile uint32_t g_once, g_periodic;

static void once_cb(void *arg)
{
    (void)arg;
    g_once++;
    demo_log("[4.3] one-shot callback fired");
}

static void periodic_cb(void *arg)
{
    (void)arg;
    g_periodic++;
}

static int experiment_43(void)
{
    demo_log("[4.3] one-shot and periodic timers");
    g_once = 0;
    g_periodic = 0;
    osTimerId_t once = osTimerNew(once_cb, osTimerOnce, NULL, NULL);
    osTimerId_t periodic =
        osTimerNew(periodic_cb, osTimerPeriodic, NULL, NULL);
    int ok = demo_check(once && periodic, "osTimerNew");
    if (once)
        ok &= demo_check(osTimerStart(once, 20) == osOK, "start one-shot");
    if (periodic)
        ok &= demo_check(osTimerStart(periodic, 10) == osOK,
                         "start periodic");
    osDelay(120);
    if (periodic)
        osTimerStop(periodic);
    if (once)
        osTimerDelete(once);
    if (periodic)
        osTimerDelete(periodic);
    ok &= demo_check(g_once == 1, "one-shot fired exactly once");
    ok &= demo_check(g_periodic >= 4, "periodic fired at least four times");
    demo_log("[4.3] %s once=%u periodic=%u", ok ? "PASS" : "FAIL",
             (unsigned)g_once, (unsigned)g_periodic);
    return ok;
}

int demo_main(a20_mcu_demo_record_t *record)
{
    demo_runtime_init(record);
    demo_log("A20OS CMSIS-RTOS2 experiments 4.1-4.5");
    if (osKernelInitialize() != osOK) {
        demo_log("osKernelInitialize failed");
        demo_finish(1);
        return 1;
    }
    if (osKernelStart() != osOK) {
        demo_log("osKernelStart failed");
        demo_finish(1);
        return 1;
    }

    int ok = 1;
    ok &= experiment_41();
    ok &= experiment_42();
    ok &= experiment_43();
    ok &= experiment_44();
    ok &= experiment_45();

    demo_log("RESULT %s checks=%u", ok ? "PASS" : "FAIL",
             (unsigned)record->checks);
    demo_finish(ok ? 0 : 1);
    return ok ? 0 : 1;
}

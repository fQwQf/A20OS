/*
 * Host self-test for the CMSIS-RTOS2 personality (POSIX backend).
 *
 * The test is intentionally behavioural: it does not inspect internals, it
 * runs the same API an application would use.  Every case maps to a CMSIS
 * object that a teaching demo or a real firmware relies on.
 */
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

#include "cmsis_os2.h"

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

/* ------------------------------------------------------------------------- */
/* Threads                                                                   */
/* ------------------------------------------------------------------------- */

static atomic_int g_counter;

static void counter_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 1000; i++)
        atomic_fetch_add(&g_counter, 1);
}

static void test_threads(void)
{
    osThreadAttr_t attr = {0};
    attr.name = "counter";
    attr.stack_size = 32768;
    attr.priority = osPriorityNormal;

    atomic_store(&g_counter, 0);
    osThreadId_t t1 = osThreadNew(counter_task, NULL, &attr);
    osThreadId_t t2 = osThreadNew(counter_task, NULL, &attr);
    CHECK(t1 && t2);
    CHECK(osThreadGetState(t1) != osThreadError);
    CHECK(strcmp(osThreadGetName(t1), "counter") == 0);
    CHECK(osThreadJoin(t1) == osOK);
    CHECK(osThreadJoin(t2) == osOK);
    CHECK(atomic_load(&g_counter) == 2000);
    CHECK(osThreadGetCount() >= 1);      /* at least the main thread */
}

/* ------------------------------------------------------------------------- */
/* Semaphore (traffic-light demo shape)                                       */
/* ------------------------------------------------------------------------- */

static osSemaphoreId_t g_sem;
static atomic_int g_sidewalks;

static void sidewalk_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 3; i++) {
        CHECK(osSemaphoreAcquire(g_sem, osWaitForever) == osOK);
        atomic_fetch_add(&g_sidewalks, 1);
        osDelay(1);
        CHECK(osSemaphoreRelease(g_sem) == osOK);
    }
}

static void test_semaphore(void)
{
    g_sem = osSemaphoreNew(1, 0, NULL);
    CHECK(g_sem != NULL);
    atomic_store(&g_sidewalks, 0);

    osThreadAttr_t attr = {0};
    attr.name = "sidewalk";
    attr.stack_size = 32768;
    osThreadId_t t = osThreadNew(sidewalk_task, NULL, &attr);
    CHECK(t != NULL);

    CHECK(osSemaphoreRelease(g_sem) == osOK);
    CHECK(osThreadJoin(t) == osOK);
    CHECK(atomic_load(&g_sidewalks) == 3);
    CHECK(osSemaphoreGetCount(g_sem) == 1);
    CHECK(osSemaphoreAcquire(g_sem, 0) == osOK);
    CHECK(osSemaphoreAcquire(g_sem, 0) == osErrorResource);
    CHECK(osSemaphoreDelete(g_sem) == osOK);
}

/* ------------------------------------------------------------------------- */
/* Mutex                                                                     */
/* ------------------------------------------------------------------------- */

static osMutexId_t g_mutex;
static long g_shared;
static atomic_int g_mutex_done;

static void mutex_task(void *arg)
{
    (void)arg;
    for (int i = 0; i < 500; i++) {
        CHECK(osMutexAcquire(g_mutex, osWaitForever) == osOK);
        long v = g_shared;
        v++;
        g_shared = v;
        CHECK(osMutexRelease(g_mutex) == osOK);
    }
    atomic_fetch_add(&g_mutex_done, 1);
}

static void test_mutex(void)
{
    osMutexAttr_t attr = {0};
    attr.attr_bits = osMutexRecursive;
    g_mutex = osMutexNew(&attr);
    CHECK(g_mutex != NULL);
    g_shared = 0;
    atomic_store(&g_mutex_done, 0);

    osThreadAttr_t tattr = {0};
    tattr.stack_size = 32768;
    osThreadId_t t[4];
    for (int i = 0; i < 4; i++)
        t[i] = osThreadNew(mutex_task, NULL, &tattr);
    for (int i = 0; i < 4; i++)
        CHECK(t[i] != NULL);
    for (int i = 0; i < 4; i++)
        CHECK(osThreadJoin(t[i]) == osOK);

    CHECK(g_shared == 2000);
    CHECK(atomic_load(&g_mutex_done) == 4);

    CHECK(osMutexAcquire(g_mutex, 0) == osOK);
    CHECK(osMutexAcquire(g_mutex, 0) == osOK);   /* recursive */
    CHECK(osMutexGetOwner(g_mutex) == osThreadGetId());
    CHECK(osMutexRelease(g_mutex) == osOK);
    CHECK(osMutexRelease(g_mutex) == osOK);
    CHECK(osMutexGetOwner(g_mutex) == NULL);

    CHECK(osMutexDelete(g_mutex) == osOK);
}

/* ------------------------------------------------------------------------- */
/* Event flags and thread flags                                              */
/* ------------------------------------------------------------------------- */

static osEventFlagsId_t g_events;
static atomic_int g_flag_hits;

static void event_task(void *arg)
{
    (void)arg;
    uint32_t got = osEventFlagsWait(g_events, 0x3, osFlagsWaitAll,
                                    osWaitForever);
    CHECK(got == 0x3);
    atomic_fetch_add(&g_flag_hits, 1);
}

static osThreadId_t g_flag_thread;

static void thread_flag_task(void *arg)
{
    (void)arg;
    uint32_t got = osThreadFlagsWait(0x10, osFlagsWaitAny, osWaitForever);
    CHECK(got == 0x10);
    atomic_fetch_add(&g_flag_hits, 1);
}

static void test_flags(void)
{
    g_events = osEventFlagsNew(NULL);
    CHECK(g_events != NULL);

    osThreadAttr_t attr = {0};
    attr.stack_size = 32768;
    osThreadId_t t = osThreadNew(event_task, NULL, &attr);
    CHECK(t != NULL);
    osDelay(5);
    CHECK(osEventFlagsSet(g_events, 0x1) == 0x1);
    CHECK(osEventFlagsSet(g_events, 0x2) == 0x3);
    CHECK(osThreadJoin(t) == osOK);

    atomic_store(&g_flag_hits, 0);
    g_flag_thread = osThreadNew(thread_flag_task, NULL, &attr);
    CHECK(g_flag_thread != NULL);
    osDelay(5);
    CHECK((osThreadFlagsSet(g_flag_thread, 0x10) & 0x10) == 0x10);
    CHECK(osThreadJoin(g_flag_thread) == osOK);

    CHECK(osEventFlagsDelete(g_events) == osOK);
}

/* ------------------------------------------------------------------------- */
/* Message queue and memory pool                                             */
/* ------------------------------------------------------------------------- */

static void test_queue(void)
{
    osMessageQueueId_t q = osMessageQueueNew(4, sizeof(int), NULL);
    CHECK(q != NULL);
    CHECK(osMessageQueueGetCapacity(q) == 4);
    CHECK(osMessageQueueGetMsgSize(q) == sizeof(int));
    CHECK(osMessageQueueGetCount(q) == 0);

    for (int i = 0; i < 4; i++)
        CHECK(osMessageQueuePut(q, &i, 0, 0) == osOK);
    CHECK(osMessageQueueGetSpace(q) == 0);
    int extra = 99;
    CHECK(osMessageQueuePut(q, &extra, 0, 0) == osErrorResource);

    for (int i = 0; i < 4; i++) {
        int v = -1;
        CHECK(osMessageQueueGet(q, &v, NULL, 0) == osOK);
        CHECK(v == i);
    }
    int v = -1;
    CHECK(osMessageQueueGet(q, &v, NULL, 0) == osErrorResource);

    /* Blocking put/get across threads. */
    for (int i = 0; i < 2; i++)
        CHECK(osMessageQueuePut(q, &i, 0, 0) == osOK);
    CHECK(osMessageQueueGetCount(q) == 2);
    CHECK(osMessageQueueReset(q) == osOK);
    CHECK(osMessageQueueGetCount(q) == 0);
    CHECK(osMessageQueueDelete(q) == osOK);

    osMemoryPoolId_t mp = osMemoryPoolNew(4, 32, NULL);
    CHECK(mp != NULL);
    CHECK(osMemoryPoolGetCapacity(mp) == 4);
    CHECK(osMemoryPoolGetBlockSize(mp) >= 32);
    CHECK(osMemoryPoolGetSpace(mp) == 4);

    void *blocks[4];
    for (int i = 0; i < 4; i++) {
        blocks[i] = osMemoryPoolAlloc(mp, 0);
        CHECK(blocks[i] != NULL);
    }
    CHECK(osMemoryPoolGetCount(mp) == 4);
    CHECK(osMemoryPoolGetSpace(mp) == 0);
    CHECK(osMemoryPoolAlloc(mp, 0) == NULL);
    for (int i = 0; i < 4; i++)
        CHECK(osMemoryPoolFree(mp, blocks[i]) == osOK);
    CHECK(osMemoryPoolFree(mp, blocks[0]) == osErrorParameter);
    CHECK(osMemoryPoolGetSpace(mp) == 4);
    CHECK(osMemoryPoolDelete(mp) == osOK);
}

/* ------------------------------------------------------------------------- */
/* Software timer (pet-feeder demo shape)                                    */
/* ------------------------------------------------------------------------- */

static atomic_int g_feeds;

static void feed_cb(void *arg)
{
    (void)arg;
    atomic_fetch_add(&g_feeds, 1);
}

static void test_timer(void)
{
    atomic_store(&g_feeds, 0);
    osTimerId_t id = osTimerNew(feed_cb, osTimerPeriodic, NULL, NULL);
    CHECK(id != NULL);
    CHECK(strcmp(osTimerGetName(id), "timer") == 0);
    CHECK(osTimerIsRunning(id) == 0);
    CHECK(osTimerStart(id, 5) == osOK);
    CHECK(osTimerIsRunning(id) == 1);

    for (int i = 0; i < 200 && atomic_load(&g_feeds) < 3; i++)
        osDelay(5);
    CHECK(atomic_load(&g_feeds) >= 3);

    CHECK(osTimerStop(id) == osOK);
    CHECK(osTimerIsRunning(id) == 0);
    int seen = atomic_load(&g_feeds);
    osDelay(30);
    CHECK(atomic_load(&g_feeds) == seen);

    CHECK(osTimerDelete(id) == osOK);

    osTimerId_t once = osTimerNew(feed_cb, osTimerOnce, NULL, NULL);
    CHECK(once != NULL);
    atomic_store(&g_feeds, 0);
    CHECK(osTimerStart(once, 5) == osOK);
    for (int i = 0; i < 200 && atomic_load(&g_feeds) < 1; i++)
        osDelay(5);
    CHECK(atomic_load(&g_feeds) == 1);
    CHECK(osTimerIsRunning(once) == 0);
    CHECK(osTimerDelete(once) == osOK);
}

/* ------------------------------------------------------------------------- */

int main(void)
{
    CHECK(osKernelInitialize() == osOK);
    CHECK(osKernelStart() == osOK);
    CHECK(osKernelGetState() == osKernelRunning);
    CHECK(osKernelGetTickFreq() > 0);

    osVersion_t ver;
    char id[32];
    CHECK(osKernelGetInfo(&ver, id, sizeof(id)) == osOK);
    CHECK(ver.api >= OS_API_VERSION);
    CHECK(strstr(id, "A20OS") != NULL);

    printf("phase: threads\n"); fflush(stdout);
    test_threads();
    printf("phase: semaphore\n"); fflush(stdout);
    test_semaphore();
    printf("phase: mutex\n"); fflush(stdout);
    test_mutex();
    printf("phase: flags\n"); fflush(stdout);
    test_flags();
    printf("phase: queue\n"); fflush(stdout);
    test_queue();
    printf("phase: timer\n"); fflush(stdout);
    test_timer();

    uint32_t before = osKernelGetTickCount();
    osDelay(20);
    CHECK(osKernelGetTickCount() - before >= 20);

    if (g_failures) {
        printf("cmsis_rtos2: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("cmsis_rtos2: all tests passed (%u Hz tick)\n", osKernelGetTickFreq());
    return 0;
}

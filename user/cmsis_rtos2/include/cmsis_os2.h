/*
 * A20OS CMSIS-RTOS2 personality.
 *
 * This header declares the Arm CMSIS-RTOS2 API surface (version 2.x) so that
 * existing CMSIS-RTOS2 applications can be rebuilt against A20OS without
 * touching their source.  It is an independent implementation: the API names,
 * types and documented semantics follow the CMSIS-RTOS2 specification, the
 * code below does not.
 *
 * Layering:
 *
 *     application
 *         |
 *     cmsis_os2.h / libcmsis_rtos2        <- this library (user space)
 *         |
 *     os_backend (narrow, see os_impl.h)
 *         |
 *     Linux ABI (pthread/futex)  or  Native ABI (thread/futex/vm_alloc)
 *         |
 *     A20OS kernel mechanisms
 *
 * The library never calls the kernel directly: everything goes through the
 * backend, which is the only part a port has to provide.  Features can be
 * compiled out with the OS_CFG_* macros so a minimal MCU image only carries
 * the objects it actually uses.
 */
#ifndef CMSIS_OS2_H_
#define CMSIS_OS2_H_

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------- */
/* Version and feature configuration                                         */
/* ------------------------------------------------------------------------- */

#define OS_API_VERSION_MAJOR   2U
#define OS_API_VERSION_MINOR   1U
#define OS_API_VERSION_PATCH   3U
#define OS_API_VERSION         ((OS_API_VERSION_MAJOR << 16) | \
                                (OS_API_VERSION_MINOR << 8)  | \
                                 OS_API_VERSION_PATCH)

#ifndef OS_CFG_THREAD
#define OS_CFG_THREAD           1
#endif
#ifndef OS_CFG_TIMER
#define OS_CFG_TIMER            1
#endif
#ifndef OS_CFG_MUTEX
#define OS_CFG_MUTEX            1
#endif
#ifndef OS_CFG_SEMAPHORE
#define OS_CFG_SEMAPHORE        1
#endif
#ifndef OS_CFG_MEMORY_POOL
#define OS_CFG_MEMORY_POOL      1
#endif
#ifndef OS_CFG_MESSAGE_QUEUE
#define OS_CFG_MESSAGE_QUEUE    1
#endif
#ifndef OS_CFG_EVENT_FLAGS
#define OS_CFG_EVENT_FLAGS      1
#endif
#ifndef OS_CFG_THREAD_FLAGS
#define OS_CFG_THREAD_FLAGS     1
#endif

/* A feature that is compiled out still exports its entry points, so a binary
 * built against a full header links; they return osErrorResource.  This keeps
 * the trim a link-time property instead of a header-swap exercise. */

/* ------------------------------------------------------------------------- */
/* Status codes                                                              */
/* ------------------------------------------------------------------------- */

typedef enum {
    osOK                    =  0,
    osError                 =  1,
    osErrorTimeout          =  2,
    osErrorResource         =  3,
    osErrorParameter        =  4,
    osErrorNoMemory         =  5,
    osErrorISR              =  6,
    osErrorISRRecursive     = -1,
    osErrorPriority         = -2,
    osStatusReserved        = 0x7FFFFFFF
} osStatus_t;

/* ------------------------------------------------------------------------- */
/* Kernel                                                                    */
/* ------------------------------------------------------------------------- */

typedef enum {
    osKernelInactive        =  0,
    osKernelReady           =  1,
    osKernelRunning         =  2,
    osKernelLocked          =  3,
    osKernelSuspended       =  4,
    osKernelError           = -1,
    osKernelReserved        = 0x7FFFFFFF
} osKernelState_t;

typedef struct {
    uint32_t api;
    uint32_t kernel;
} osVersion_t;

osStatus_t      osKernelInitialize(void);
osStatus_t      osKernelStart(void);
osKernelState_t osKernelGetState(void);
osStatus_t      osKernelGetInfo(osVersion_t *version, char *id_buf,
                                uint32_t id_size);
uint32_t        osKernelGetTickCount(void);
uint32_t        osKernelGetTickFreq(void);
uint32_t        osKernelGetSysTimerCount(void);
uint32_t        osKernelGetSysTimerFreq(void);
uint32_t        osKernelGetVersion(void);

int32_t         osKernelLock(void);
int32_t         osKernelUnlock(void);
int32_t         osKernelRestoreLock(int32_t lock);

uint32_t        osKernelSuspend(void);
void            osKernelResume(uint32_t sleep_ticks);

void            osDelay(uint32_t ticks);
osStatus_t      osDelayUntil(uint32_t ticks);

/* ------------------------------------------------------------------------- */
/* Threads                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum {
    osPriorityNone          =  0,
    osPriorityIdle          =  1,
    osPriorityLow           =  8,
    osPriorityLow1          =  9,
    osPriorityLow2          = 10,
    osPriorityLow3          = 11,
    osPriorityLow4          = 12,
    osPriorityLow5          = 13,
    osPriorityLow6          = 14,
    osPriorityLow7          = 15,
    osPriorityBelowNormal   = 16,
    osPriorityBelowNormal1  = 17,
    osPriorityBelowNormal2  = 18,
    osPriorityBelowNormal3  = 19,
    osPriorityBelowNormal4  = 20,
    osPriorityBelowNormal5  = 21,
    osPriorityBelowNormal6  = 22,
    osPriorityBelowNormal7  = 23,
    osPriorityNormal        = 24,
    osPriorityNormal1       = 25,
    osPriorityNormal2       = 26,
    osPriorityNormal3       = 27,
    osPriorityNormal4       = 28,
    osPriorityNormal5       = 29,
    osPriorityNormal6       = 30,
    osPriorityNormal7       = 31,
    osPriorityAboveNormal   = 32,
    osPriorityAboveNormal1  = 33,
    osPriorityAboveNormal2  = 34,
    osPriorityAboveNormal3  = 35,
    osPriorityAboveNormal4  = 36,
    osPriorityAboveNormal5  = 37,
    osPriorityAboveNormal6  = 38,
    osPriorityAboveNormal7  = 39,
    osPriorityHigh          = 40,
    osPriorityHigh1         = 41,
    osPriorityHigh2         = 42,
    osPriorityHigh3         = 43,
    osPriorityHigh4         = 44,
    osPriorityHigh5         = 45,
    osPriorityHigh6         = 46,
    osPriorityHigh7         = 47,
    osPriorityRealtime      = 48,
    osPriorityRealtime1     = 49,
    osPriorityRealtime2     = 50,
    osPriorityRealtime3     = 51,
    osPriorityRealtime4     = 52,
    osPriorityRealtime5     = 53,
    osPriorityRealtime6     = 54,
    osPriorityRealtime7     = 55,
    osPriorityISR           = 56,
    osPriorityError         = 57,
    osPriorityReserved      = 0x7FFFFFFF
} osPriority_t;

typedef enum {
    osThreadInactive        =  0,
    osThreadReady           =  1,
    osThreadRunning         =  2,
    osThreadBlocked         =  3,
    osThreadTerminated      =  4,
    osThreadError           = -1,
    osThreadReserved        = 0x7FFFFFFF
} osThreadState_t;

typedef struct os_thread_cb *osThreadId_t;
typedef void (*osThreadFunc_t)(void *argument);

typedef struct {
    const char                   *name;
    uint32_t                      attr_bits;
    void                         *cb_mem;
    uint32_t                      cb_size;
    void                         *stack_mem;
    uint32_t                      stack_size;
    osPriority_t                  priority;
    uint32_t                      tz_module;
    uint32_t                      reserved;
} osThreadAttr_t;

#define osThreadJoinable        (1U << 0)
#define osThreadDetached        (1U << 1)

osThreadId_t osThreadNew(osThreadFunc_t func, void *argument,
                         const osThreadAttr_t *attr);
const char  *osThreadGetName(osThreadId_t thread_id);
osThreadId_t osThreadGetId(void);
osThreadState_t osThreadGetState(osThreadId_t thread_id);
osStatus_t   osThreadSetPriority(osThreadId_t thread_id, osPriority_t priority);
osPriority_t osThreadGetPriority(osThreadId_t thread_id);
osStatus_t   osThreadYield(void);
osStatus_t   osThreadSuspend(osThreadId_t thread_id);
osStatus_t   osThreadResume(osThreadId_t thread_id);
osStatus_t   osThreadDetach(osThreadId_t thread_id);
osStatus_t   osThreadJoin(osThreadId_t thread_id);
void         osThreadExit(void);
osStatus_t   osThreadTerminate(osThreadId_t thread_id);
uint32_t     osThreadGetStackSize(osThreadId_t thread_id);
uint32_t     osThreadGetStackSpace(osThreadId_t thread_id);
uint32_t     osThreadGetCount(void);
uint32_t     osThreadEnumerate(osThreadId_t *thread_array,
                               uint32_t array_items);
osStatus_t   osThreadSetAffinity(osThreadId_t thread_id, uint32_t cpu_set);
uint32_t     osThreadGetAffinity(osThreadId_t thread_id);

/* ------------------------------------------------------------------------- */
/* Thread flags                                                              */
/* ------------------------------------------------------------------------- */

#define osFlagsWaitAny          0x00000000U
#define osFlagsWaitAll          0x00000001U
#define osFlagsNoClear          0x00000002U
#define osFlagsMask             0x0000000FU

#define osFlagsError            0x80000000U
#define osFlagsErrorUnknown     0xFFFFFFFFU
#define osFlagsErrorTimeout     0xFFFFFFFEU
#define osFlagsErrorResource    0xFFFFFFFDU
#define osFlagsErrorParameter   0xFFFFFFFCU
#define osFlagsErrorISR         0xFFFFFFFAU

uint32_t    osThreadFlagsSet(osThreadId_t thread_id, uint32_t flags);
uint32_t    osThreadFlagsClear(uint32_t flags);
uint32_t    osThreadFlagsGet(void);
uint32_t    osThreadFlagsWait(uint32_t flags, uint32_t options,
                              uint32_t timeout);

/* ------------------------------------------------------------------------- */
/* Software timers                                                           */
/* ------------------------------------------------------------------------- */

typedef struct os_timer_cb *osTimerId_t;
typedef void (*osTimerFunc_t)(void *argument);

typedef enum {
    osTimerOnce             = 0,
    osTimerPeriodic         = 1
} osTimerType_t;

typedef struct {
    const char                   *name;
    uint32_t                      attr_bits;
    void                         *cb_mem;
    uint32_t                      cb_size;
} osTimerAttr_t;

osTimerId_t osTimerNew(osTimerFunc_t func, osTimerType_t type, void *argument,
                       const osTimerAttr_t *attr);
const char *osTimerGetName(osTimerId_t timer_id);
osStatus_t  osTimerStart(osTimerId_t timer_id, uint32_t ticks);
osStatus_t  osTimerStop(osTimerId_t timer_id);
uint32_t    osTimerIsRunning(osTimerId_t timer_id);
osStatus_t  osTimerDelete(osTimerId_t timer_id);

/* ------------------------------------------------------------------------- */
/* Mutexes                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct os_mutex_cb *osMutexId_t;

typedef struct {
    const char                   *name;
    uint32_t                      attr_bits;
    void                         *cb_mem;
    uint32_t                      cb_size;
} osMutexAttr_t;

#define osMutexRecursive        (1U << 0)
#define osMutexPrioInherit      (1U << 1)
#define osMutexRobust           (1U << 3)

osMutexId_t osMutexNew(const osMutexAttr_t *attr);
const char *osMutexGetName(osMutexId_t mutex_id);
osStatus_t  osMutexAcquire(osMutexId_t mutex_id, uint32_t timeout);
osStatus_t  osMutexRelease(osMutexId_t mutex_id);
osThreadId_t osMutexGetOwner(osMutexId_t mutex_id);
osStatus_t  osMutexDelete(osMutexId_t mutex_id);

/* ------------------------------------------------------------------------- */
/* Semaphores                                                                */
/* ------------------------------------------------------------------------- */

typedef struct os_semaphore_cb *osSemaphoreId_t;

typedef struct {
    const char                   *name;
    uint32_t                      attr_bits;
    void                         *cb_mem;
    uint32_t                      cb_size;
} osSemaphoreAttr_t;

osSemaphoreId_t osSemaphoreNew(uint32_t max_count, uint32_t initial_count,
                               const osSemaphoreAttr_t *attr);
const char *osSemaphoreGetName(osSemaphoreId_t semaphore_id);
osStatus_t  osSemaphoreAcquire(osSemaphoreId_t semaphore_id, uint32_t timeout);
osStatus_t  osSemaphoreRelease(osSemaphoreId_t semaphore_id);
uint32_t    osSemaphoreGetCount(osSemaphoreId_t semaphore_id);
osStatus_t  osSemaphoreDelete(osSemaphoreId_t semaphore_id);

/* ------------------------------------------------------------------------- */
/* Memory pools                                                              */
/* ------------------------------------------------------------------------- */

typedef struct os_mempool_cb *osMemoryPoolId_t;

typedef struct {
    const char                   *name;
    uint32_t                      attr_bits;
    void                         *cb_mem;
    uint32_t                      cb_size;
} osMemoryPoolAttr_t;

osMemoryPoolId_t osMemoryPoolNew(uint32_t block_count, uint32_t block_size,
                                 const osMemoryPoolAttr_t *attr);
const char *osMemoryPoolGetName(osMemoryPoolId_t mp_id);
void *osMemoryPoolAlloc(osMemoryPoolId_t mp_id, uint32_t timeout);
osStatus_t osMemoryPoolFree(osMemoryPoolId_t mp_id, void *block);
uint32_t osMemoryPoolGetCapacity(osMemoryPoolId_t mp_id);
uint32_t osMemoryPoolGetBlockSize(osMemoryPoolId_t mp_id);
uint32_t osMemoryPoolGetCount(osMemoryPoolId_t mp_id);
uint32_t osMemoryPoolGetSpace(osMemoryPoolId_t mp_id);
osStatus_t osMemoryPoolDelete(osMemoryPoolId_t mp_id);

/* ------------------------------------------------------------------------- */
/* Message queues                                                            */
/* ------------------------------------------------------------------------- */

typedef struct os_messagequeue_cb *osMessageQueueId_t;

typedef struct {
    const char                   *name;
    uint32_t                      attr_bits;
    void                         *cb_mem;
    uint32_t                      cb_size;
} osMessageQueueAttr_t;

osMessageQueueId_t osMessageQueueNew(uint32_t msg_count, uint32_t msg_size,
                                     const osMessageQueueAttr_t *attr);
const char *osMessageQueueGetName(osMessageQueueId_t mq_id);
osStatus_t  osMessageQueuePut(osMessageQueueId_t mq_id, const void *msg_ptr,
                              uint8_t msg_prio, uint32_t timeout);
osStatus_t  osMessageQueueGet(osMessageQueueId_t mq_id, void *msg_ptr,
                              uint8_t *msg_prio, uint32_t timeout);
uint32_t    osMessageQueueGetCapacity(osMessageQueueId_t mq_id);
uint32_t    osMessageQueueGetMsgSize(osMessageQueueId_t mq_id);
uint32_t    osMessageQueueGetCount(osMessageQueueId_t mq_id);
uint32_t    osMessageQueueGetSpace(osMessageQueueId_t mq_id);
osStatus_t  osMessageQueueReset(osMessageQueueId_t mq_id);
osStatus_t  osMessageQueueDelete(osMessageQueueId_t mq_id);

/* ------------------------------------------------------------------------- */
/* Event flags                                                               */
/* ------------------------------------------------------------------------- */

typedef struct os_eventflags_cb *osEventFlagsId_t;

typedef struct {
    const char                   *name;
    uint32_t                      attr_bits;
    void                         *cb_mem;
    uint32_t                      cb_size;
} osEventFlagsAttr_t;

osEventFlagsId_t osEventFlagsNew(const osEventFlagsAttr_t *attr);
const char *osEventFlagsGetName(osEventFlagsId_t ef_id);
uint32_t    osEventFlagsSet(osEventFlagsId_t ef_id, uint32_t flags);
uint32_t    osEventFlagsClear(osEventFlagsId_t ef_id, uint32_t flags);
uint32_t    osEventFlagsGet(osEventFlagsId_t ef_id);
uint32_t    osEventFlagsWait(osEventFlagsId_t ef_id, uint32_t flags,
                             uint32_t options, uint32_t timeout);
osStatus_t  osEventFlagsDelete(osEventFlagsId_t ef_id);

/* ------------------------------------------------------------------------- */
/* Constants                                                                 */
/* ------------------------------------------------------------------------- */

#define osWaitForever           0xFFFFFFFFU
#ifdef __cplusplus
}
#endif

#endif /* CMSIS_OS2_H_ */

/*
 * Internal definitions for the A20OS CMSIS-RTOS2 personality.
 *
 * The library is split into a portable core (this file plus os_*.c) and one
 * backend per host ABI.  The backend interface is deliberately tiny and
 * primitive: threads, a monotonic clock, an allocator, a recursive kernel
 * lock and one "blocking queue" (mutex + timed wait) per object.  Everything
 * else — mutex ownership, semaphore counting, queues, flags, pools, software
 * timers — is built in the core on top of those primitives, which keeps the
 * porting surface small and the semantics identical across backends.
 */
#ifndef OS_IMPL_H_
#define OS_IMPL_H_

#include <stddef.h>
#include <stdint.h>

#include "cmsis_os2.h"

/* The core is freestanding: it must build for an MCU image with no libc.
 * Keep the few string/memory helpers and errno values local. */
#define OS_EIO        5
#define OS_EAGAIN    11
#define OS_ENOMEM    12
#define OS_EINVAL    22
#define OS_ESRCH      3
#define OS_EPERM      1
#define OS_ENOSYS    38
#define OS_ETIMEDOUT 110

static inline void os_memzero(void *dst, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    while (n--)
        *d++ = 0;
}

static inline void os_memcopy(void *dst, const void *src, size_t n)
{
    unsigned char *d = (unsigned char *)dst;
    const unsigned char *s = (const unsigned char *)src;
    while (n--)
        *d++ = *s++;
}

static inline size_t os_strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

#define OS_CFG_NAME_MAX     16U
#define OS_CFG_DEFAULT_STACK_SIZE 16384U

#ifndef OS_CFG_TIMER_THREAD_STACK
#define OS_CFG_TIMER_THREAD_STACK 8192U
#endif
#ifndef OS_CFG_TIMER_MAX
#define OS_CFG_TIMER_MAX    64U
#endif

/* ------------------------------------------------------------------------- */
/* Backend contract                                                          */
/* ------------------------------------------------------------------------- */

typedef struct os_bq os_bq_t;

int      osb_init(void);
void    *osb_alloc(size_t size);
void     osb_free(void *ptr);
uint64_t osb_time_ns(void);
uint32_t osb_tick_freq(void);

int      osb_thread_start(osThreadId_t t);
void     osb_thread_exit(void);
int      osb_thread_join(osThreadId_t t);
int      osb_thread_detach(osThreadId_t t);
void     osb_thread_yield(void);
int      osb_thread_sleep_ns(uint64_t ns);
int      osb_thread_suspend(osThreadId_t t);
int      osb_thread_resume(osThreadId_t t);
int      osb_thread_terminate(osThreadId_t t);
osThreadId_t osb_thread_self(void);
void     osb_thread_set_self(osThreadId_t t);
int      osb_thread_set_priority(osThreadId_t t, osPriority_t prio);
osPriority_t osb_thread_get_priority(osThreadId_t t);
int      osb_thread_set_affinity(osThreadId_t t, uint32_t cpu_set);
uint32_t osb_thread_get_affinity(osThreadId_t t);
uint32_t osb_thread_stack_space(osThreadId_t t);

int      osb_kernel_lock(void);
int      osb_kernel_unlock(void);
int      osb_kernel_restore(int depth);

os_bq_t *osb_bq_create(void);
void     osb_bq_destroy(os_bq_t *q);
void     osb_bq_lock(os_bq_t *q);
void     osb_bq_unlock(os_bq_t *q);
int      osb_bq_wait_until(os_bq_t *q, uint64_t deadline_ns);
void     osb_bq_wake(os_bq_t *q, unsigned count);
void     osb_bq_wake_all(os_bq_t *q);

/* The core calls the backend entry trampoline with the thread object; the
 * backend must arrange for `os_thread_entry` to run on the new thread. */
void     os_thread_entry(void *arg);

/* ------------------------------------------------------------------------- */
/* Objects                                                                   */
/* ------------------------------------------------------------------------- */

#define OS_MAGIC            0x4F533258u   /* "OS2X" */
#define OS_FOREVER_NS       UINT64_MAX

enum os_kind {
    OS_KIND_THREAD = 1,
    OS_KIND_TIMER,
    OS_KIND_MUTEX,
    OS_KIND_SEM,
    OS_KIND_MP,
    OS_KIND_MQ,
    OS_KIND_EF,
};

typedef struct os_object {
    uint32_t magic;
    uint16_t kind;
    uint16_t static_cb;
    char     name[OS_CFG_NAME_MAX];
    os_bq_t *q;                 /* protects the object state and its waiters */
} os_object_t;

struct os_thread_cb {
    os_object_t     obj;
    void           *bhandle;
    osThreadFunc_t  func;
    void           *arg;
    void           *stack_mem;
    uint32_t        stack_size;
    int             static_stack;
    void           *owned_stack;   /* allocated by the backend, freed on release */
    osPriority_t    priority;
    uint32_t        attr_bits;
    volatile int32_t state;
    volatile uint32_t flags;
    volatile int32_t suspend_req;
    int             detached;
    uint32_t        exit_code;
    volatile uint32_t exit_word;   /* join completion, woken through futex */
    struct os_thread_cb *next;
};

struct os_timer_cb {
    os_object_t     obj;
    osTimerFunc_t   func;
    void           *arg;
    volatile int32_t running;
    volatile int32_t type;
    uint32_t        ticks;
    uint64_t        next_ns;
    struct os_timer_cb *next;
};

struct os_mutex_cb {
    os_object_t     obj;
    osThreadId_t    owner;
    uint32_t        count;      /* recursion depth */
    uint32_t        attr;
};

struct os_semaphore_cb {
    os_object_t     obj;
    uint32_t        count;
    uint32_t        max_count;
};

struct os_eventflags_cb {
    os_object_t     obj;
    uint32_t        flags;
};

struct os_mempool_cb {
    os_object_t     obj;
    uint8_t        *pool;
    uint32_t       *free_list;
    uint32_t        capacity;
    uint32_t        block_size;
    uint32_t        used;
    uint32_t        free_head;
};

struct os_messagequeue_cb {
    os_object_t     obj;
    uint8_t        *buf;
    uint32_t        capacity;
    uint32_t        msg_size;
    uint32_t        count;
    uint32_t        head;
    uint32_t        tail;
};

/* ------------------------------------------------------------------------- */
/* Core helpers shared by the os_*.c files                                   */
/* ------------------------------------------------------------------------- */

int      os_core_init(void);
osThreadId_t os_current_thread(void);
uint64_t os_deadline_ns(uint32_t timeout_ticks);
uint64_t os_ticks_to_ns(uint64_t ticks);
uint64_t os_ns_to_ticks(uint64_t ns);
void     os_name_store(char dst[OS_CFG_NAME_MAX], const char *src);
int      os_status_to_errno(osStatus_t st);

int      os_thread_registry_add(osThreadId_t t);
void     os_thread_registry_remove(osThreadId_t t);
uint32_t os_thread_registry_count(void);
uint32_t os_thread_registry_enum(osThreadId_t *arr, uint32_t n);
void     os_thread_release(osThreadId_t t);

int      os_timer_core_init(void);
void     os_timer_service_start(void);
void     os_reg_lock(void);
void     os_reg_unlock(void);

#endif /* OS_IMPL_H_ */

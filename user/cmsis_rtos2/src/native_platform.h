/*
 * Platform calls used by the Native backend.
 *
 * The backend implements CMSIS policy (futex queue, object lifetime, thread
 * bookkeeping) on top of this fixed, small call set.  Two implementations
 * exist:
 *
 *   src/native_platform_a20.c   the real A20 Native ABI (freestanding)
 *   tests/native_platform_host.c pthread/futex version for the host test
 *
 * Keeping the set explicit is what makes the trim checkable: a backend that
 * needs a new primitive has to add it here, and the manifest has to name it.
 */
#ifndef NATIVE_PLATFORM_H_
#define NATIVE_PLATFORM_H_

#include <stddef.h>
#include <stdint.h>

enum osn_call {
    OSN_CLOCK_GET = 0,
    OSN_VM_ALLOC,
    OSN_THREAD_CREATE,
    OSN_THREAD_EXIT,
    OSN_THREAD_SLEEP,
    OSN_THREAD_YIELD,
    OSN_TASK_KILL,
    OSN_TASK_SET_SCHED,
    OSN_TASK_GET_SCHED,
    OSN_SCHED_LOCK,
    OSN_HANDLE_CLOSE,
    OSN_FUTEX_WAIT,
    OSN_FUTEX_WAKE,
    OSN_ATOMIC32,
    OSN_CALL_COUNT
};

/* Atomic operations, numbered like the Native ABI. */
#define OSN_ATOMIC_LOAD   0u
#define OSN_ATOMIC_STORE  1u
#define OSN_ATOMIC_XCHG   2u
#define OSN_ATOMIC_CAS    3u

/* Scheduling fields, numbered like the Native ABI. */
#define OSN_SCHED_POLICY   (1u << 0)
#define OSN_SCHED_PRIORITY (1u << 1)
#define OSN_SCHED_AFFINITY (1u << 2)
#define OSN_SCHED_DONATE   (1u << 4)
#define OSN_SCHED_UNDONATE (1u << 5)

/* Kernel scheduler-lock operations (osKernelLock). */
#define OSN_LOCK_ACQUIRE 0
#define OSN_LOCK_RELEASE 1
#define OSN_LOCK_RESTORE 2

#define OSN_SCHED_NORMAL 0
#define OSN_SCHED_FIFO   1
#define OSN_SCHED_RR     2

void     osn_platform_init(void);
/* Smallest stack the platform can actually run; 0 means "any". */
size_t   osn_stack_grow(size_t requested);
int      osn_clock_get_ns(uint64_t *out_ns);
int      osn_vm_alloc(size_t size, void **out_ptr);
void     osn_vm_free(void *ptr);
/*
 * The scheduling arguments are applied before the new thread is published.
 * Passing 0 for policy/priority means "inherit the creator".
 */
int      osn_thread_create(void (*entry)(void *), void *arg,
                           void *stack, size_t stack_size,
                           int32_t policy, int32_t priority,
                           void **out_handle);
void     osn_thread_exit(void);
/* Wait for a backend thread to be really gone.  The host implementation
 * joins the pthread; the A20 backend returns immediately because the task
 * exits inside the thread_exit syscall. */
void     osn_thread_reap(void *handle);
int      osn_thread_sleep_ns(uint64_t ns);
void     osn_thread_yield(void);
int      osn_task_kill(void *handle);
/* handle == NULL names the calling thread on the Native backend. */
int      osn_task_set_sched(void *handle, uint32_t fields, int32_t policy,
                            int32_t priority, uint32_t affinity);
int      osn_task_get_sched(void *handle, int32_t *policy, int32_t *priority);
int      osn_sched_lock(int32_t mode, int32_t depth);
int      osn_handle_close(void *handle);

/* Futex word wait/wake; timeout is relative nanoseconds (0 = no wait,
 * UINT64_MAX = forever).  Returns 0 on wake, -1 on timeout. */
int      osn_futex_wait(uint32_t *addr, uint32_t expected, uint64_t timeout_ns);
int      osn_futex_wake(uint32_t *addr, uint32_t count);

/* Returns the observed word; `exchanged` reports CAS success. */
uint32_t osn_atomic32(uint32_t *addr, uint32_t op, uint32_t expected,
                      uint32_t desired, int *exchanged);

const char *osn_call_name(int call);
unsigned    osn_call_count(int call);

#endif /* NATIVE_PLATFORM_H_ */

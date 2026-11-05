/*
 * Native backend platform calls implemented on the A20 Native ABI.
 *
 * This is the only file in the personality that knows A20 syscall names; it
 * is compiled freestanding for the target and is not part of the host test.
 */
#include "os_impl.h"
#include "native_platform.h"

#include "a20_syscall.h"
#include "a20_types.h"
#include "a20_task.h"
#include "a20_mem.h"
#include "a20_handle.h"
#include "a20_sync.h"
#include "a20_clock.h"

extern void *a20_malloc(uint64_t size);
extern void  a20_free(void *ptr);

static unsigned g_calls[OSN_CALL_COUNT];

static const char *const g_call_names[OSN_CALL_COUNT] = {
    "clock_get",
    "vm_alloc",
    "thread_create",
    "thread_exit",
    "thread_sleep",
    "thread_yield",
    "task_kill",
    "task_set_sched",
    "task_get_sched",
    "task_suspend",
    "task_resume",
    "kernel_suspend",
    "sched_lock",
    "task_self",
    "handle_close",
    "futex_wait",
    "futex_wake",
    "atomic32",
};

void osn_platform_init(void) { }

size_t osn_stack_grow(size_t requested) { return requested; }

int osn_clock_get_ns(uint64_t *out_ns)
{
    g_calls[OSN_CLOCK_GET]++;
    a20_time_ns_t ns = 0;
    if (a20_clock_get(A20_CLOCK_MONOTONIC, &ns) != A20_OK)
        return -1;
    *out_ns = ns;
    return 0;
}

int osn_vm_alloc(size_t size, void **out_ptr)
{
    g_calls[OSN_VM_ALLOC]++;
    void *p = a20_malloc(size);
    if (!p)
        return -1;
    *out_ptr = p;
    return 0;
}

void osn_vm_free(void *ptr) { a20_free(ptr); }

int osn_vm_alloc_page_aligned(size_t size, void **out_ptr)
{
    g_calls[OSN_VM_ALLOC]++;
    uint64_t pages = (size + 4095U) / 4096U;
    if (!pages)
        pages = 1;
    uint64_t addr = 0;
    if (a20_vm_alloc_pages(pages, 3, &addr) != A20_OK)
        return -1;
    *out_ptr = (void *)(uintptr_t)addr;
    return 0;
}

int osn_vm_unmap(void *ptr, size_t size)
{
    g_calls[OSN_VM_UNMAP]++;
    return a20_vm_unmap((uint64_t)(uintptr_t)ptr, (uint64_t)size) == A20_OK
               ? 0 : -1;
}

int osn_thread_create(void (*entry)(void *), void *arg, void *stack,
                      size_t stack_size, int32_t policy, int32_t priority,
                      void **out_handle)
{
    g_calls[OSN_THREAD_CREATE]++;
    a20_thread_create_args_t args = {0};
    args.size = sizeof(args);
    args.version = 1;
    args.entry = (uint64_t)(uintptr_t)entry;
    args.arg = (uint64_t)(uintptr_t)arg;
    /* The Native ABI names the 16-byte aligned stack top; the backend's
     * allocator returns the low address. */
    args.stack_base = (uint64_t)(uintptr_t)stack + stack_size;
    args.stack_size = stack_size;
    if (policy && priority > 0) {
        args.sched_flags = A20_SCHED_POLICY | A20_SCHED_PRIORITY;
        args.sched_policy = policy;
        args.sched_priority = priority;
    }
    /* sys_a20_thread_create returns the handle itself (0 is a valid slot),
     * while the backend uses a void* where NULL means "no handle".  Bias by
     * one so handle 0 stays representable. */
    int64_t rc = a20_thread_create(&args);
    if (rc < 0 || args.out_thread == A20_HANDLE_NULL)
        return -1;
    *out_handle = (void *)(uintptr_t)(args.out_thread + 1);
    return 0;
}

void osn_thread_exit(void)
{
    g_calls[OSN_THREAD_EXIT]++;
    a20_thread_exit(0);
}

void osn_thread_reap(void *handle)
{
    (void)handle;
}

int osn_thread_sleep_ns(uint64_t ns)
{
    g_calls[OSN_THREAD_SLEEP]++;
    a20_time_t d = {.secs = ns / 1000000000ULL,
                    .nsecs = ns % 1000000000ULL};
    return a20_thread_sleep(d) == A20_OK ? 0 : -1;
}

void osn_thread_yield(void)
{
    g_calls[OSN_THREAD_YIELD]++;
    (void)a20_thread_yield();
}

int osn_task_kill(void *handle)
{
    g_calls[OSN_TASK_KILL]++;
    return a20_task_kill((a20_handle_t)((uintptr_t)handle - 1), 0) == A20_OK ? 0 : -1;
}

int osn_task_set_sched(void *handle, uint32_t fields, int32_t policy,
                       int32_t priority, uint32_t affinity)
{
    g_calls[OSN_TASK_SET_SCHED]++;
    a20_sched_args_t args = {0};
    args.size = sizeof(args);
    args.version = 1;
    /* NULL names the calling task on the Native backend. */
    a20_handle_t target = handle
                              ? (a20_handle_t)((uintptr_t)handle - 1)
                              : A20_HANDLE_NULL;
    args.task = target;
    args.flags = fields;
    args.policy = policy;
    args.priority = priority;
    args.affinity = affinity;
    args.affinity_size = sizeof(affinity);
    return a20_sched_set(target, &args) == A20_OK ? 0 : -1;
}

int osn_task_get_sched(void *handle, int32_t *policy, int32_t *priority,
                       uint32_t *affinity)
{
    g_calls[OSN_TASK_GET_SCHED]++;
    if (!policy || !priority)
        return -1;
    a20_sched_args_t args = {0};
    args.size = sizeof(args);
    args.version = 1;
    a20_handle_t target = handle
                              ? (a20_handle_t)((uintptr_t)handle - 1)
                              : A20_HANDLE_NULL;
    if (a20_sched_get(target, &args) != A20_OK)
        return -1;
    *policy = args.policy;
    *priority = args.priority;
    if (affinity)
        *affinity = (uint32_t)args.affinity;
    return 0;
}

int osn_task_suspend(void *handle)
{
    g_calls[OSN_TASK_SUSPEND]++;
    a20_handle_t target = handle
                              ? (a20_handle_t)((uintptr_t)handle - 1)
                              : A20_HANDLE_NULL;
    return a20_task_suspend(target) == A20_OK ? 0 : -1;
}

int osn_task_resume(void *handle)
{
    g_calls[OSN_TASK_RESUME]++;
    a20_handle_t target = handle
                              ? (a20_handle_t)((uintptr_t)handle - 1)
                              : A20_HANDLE_NULL;
    return a20_task_resume(target) == A20_OK ? 0 : -1;
}

uint32_t osn_kernel_suspend(void)
{
    g_calls[OSN_KERNEL_SUSPEND]++;
    int64_t ticks = a20_kernel_idle();
    return ticks > 0 ? (uint32_t)ticks : 0;
}

int osn_sched_lock(int32_t mode, int32_t depth)
{
    g_calls[OSN_SCHED_LOCK]++;
    int64_t rc = a20_sched_lock(mode, depth);
    return rc < 0 ? -1 : (int)rc;
}

int osn_thread_self_handle(void **out_handle)
{
    g_calls[OSN_TASK_SELF]++;
    int64_t h = a20_task_self();
    if (h < 0 || h == A20_HANDLE_NULL)
        return -1;
    *out_handle = (void *)(uintptr_t)(h + 1);
    return 0;
}

int osn_handle_close(void *handle)
{
    g_calls[OSN_HANDLE_CLOSE]++;
    return a20_hdl_close((a20_handle_t)((uintptr_t)handle - 1)) == A20_OK ? 0 : -1;
}

int osn_futex_wait(uint32_t *addr, uint32_t expected, uint64_t timeout_ns)
{
    g_calls[OSN_FUTEX_WAIT]++;
    uint64_t t = timeout_ns == UINT64_MAX ? (UINT64_MAX / 4U) : timeout_ns;
    return a20_futex_wait(addr, expected, t) == A20_OK ? 0 : -1;
}

int osn_futex_wake(uint32_t *addr, uint32_t count)
{
    g_calls[OSN_FUTEX_WAKE]++;
    return a20_futex_wake(addr, count, NULL) == A20_OK ? 0 : -1;
}

uint32_t osn_atomic32(uint32_t *addr, uint32_t op, uint32_t expected,
                      uint32_t desired, int *exchanged)
{
    g_calls[OSN_ATOMIC32]++;
    uint32_t observed = 0, did = 0;
    (void)a20_atomic32(addr, op, expected, desired, &observed, &did);
    if (exchanged)
        *exchanged = did;
    return observed;
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

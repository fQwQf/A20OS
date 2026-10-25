/* Scheduler, allocation and synchronization services for the AML interpreter.
 * CPU instructions and firmware address-space accesses belong to arch/. */
#include "core/types.h"
#include <uacpi/kernel_api.h>
#include "core/cpu.h"
#include "core/errno.h"
#include "core/klog.h"
#include "core/panic.h"
#include "core/sync.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "mm/mm.h"
#include "proc/proc.h"

void *uacpi_kernel_alloc(uacpi_size size) {
    return size ? kmalloc(size) : NULL;
}
void uacpi_kernel_free(void *p) {
    kfree(p);
}
uacpi_u64 uacpi_kernel_get_nanoseconds_since_boot(void) {
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    return ts[0] * 1000000000ULL + ts[1];
}
void uacpi_kernel_stall(uacpi_u8 usec) {
    uint64_t start = uacpi_kernel_get_nanoseconds_since_boot();
    while (uacpi_kernel_get_nanoseconds_since_boot() - start <
           (uint64_t)usec * 1000)
        cpu_relax();
}
void uacpi_kernel_sleep(uacpi_u64 msec) {
    while (msec) {
        unsigned chunk = msec > 1000 ? 1000 : (unsigned)msec;
        if (proc_current())
            proc_sleep_until(timer_get_ticks() + MS_TO_TICKS(chunk));
        else {
            uint64_t start = uacpi_kernel_get_nanoseconds_since_boot();
            while (uacpi_kernel_get_nanoseconds_since_boot() - start <
                   (uint64_t)chunk * 1000000)
                cpu_relax();
        }
        msec -= chunk;
    }
}

typedef struct acpi_semaphore {
    spinlock_t lock;
    uint32_t units;
    wait_queue_t waiters;
} acpi_semaphore_t;

uacpi_handle uacpi_kernel_create_semaphore(uacpi_u32 initial) {
    acpi_semaphore_t *sem = kmalloc(sizeof(*sem));
    if (!sem)
        return NULL;
    spin_init(&sem->lock);
    sem->units = initial;
    wait_queue_init(&sem->waiters);
    return sem;
}
void uacpi_kernel_free_semaphore(uacpi_handle sem) {
    kfree(sem);
}

uacpi_status uacpi_kernel_wait_for_semaphore(uacpi_handle handle,
                                             uacpi_u16 timeout) {
    acpi_semaphore_t *sem = handle;
    if (!sem)
        return UACPI_STATUS_INVALID_ARGUMENT;
    uint64_t deadline =
        timeout == 0xFFFF ? 0 : timer_get_ticks() + MS_TO_TICKS(timeout);
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&sem->lock);
        if (sem->units) {
            sem->units--;
            spin_unlock_irqrestore(&sem->lock, flags);
            return UACPI_STATUS_OK;
        }
        spin_unlock_irqrestore(&sem->lock, flags);
        if (!timeout || (deadline && timer_get_ticks() >= deadline))
            return UACPI_STATUS_TIMEOUT;
        if (!proc_current()) {
            cpu_relax();
            continue;
        }
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, deadline);
        if (!token.task || !token.seq)
            return UACPI_STATUS_INTERNAL_ERROR;
        wait_queue_entry_t entry = {0};
        flags = spin_lock_irqsave(&sem->lock);
        if (sem->units) {
            sem->units--;
            spin_unlock_irqrestore(&sem->lock, flags);
            proc_park_cancel(token);
            proc_park_finish(token);
            return UACPI_STATUS_OK;
        }
        bool linked = wait_queue_link(&sem->waiters, &entry, token, 0);
        spin_unlock_irqrestore(&sem->lock, flags);
        proc_wake_reason_t why = PROC_WAKE_NONE;
        if (linked)
            why = proc_park_commit(token);
        else
            proc_park_cancel(token);
        wait_queue_unlink(&sem->waiters, &entry);
        proc_park_finish(token);
        if (why == PROC_WAKE_TIMEOUT)
            return UACPI_STATUS_TIMEOUT;
        if (why == PROC_WAKE_TIMEOUT_CAPACITY)
            return UACPI_STATUS_INTERNAL_ERROR;
    }
}
void uacpi_kernel_signal_semaphore(uacpi_handle handle) {
    acpi_semaphore_t *sem = handle;
    if (!sem)
        return;
    uint64_t flags = spin_lock_irqsave(&sem->lock);
    if (sem->units == (uint32_t)-1)
        panic("ACPI semaphore counter overflow");
    sem->units++;
    spin_unlock_irqrestore(&sem->lock, flags);
    wait_queue_wake_one(&sem->waiters, 0, PROC_WAKE_EVENT);
}

uacpi_thread_id uacpi_kernel_get_thread_id(void) {
    task_t *task = proc_current();
    /* Early boot executes only on the boot CPU; this static identity never
     * collides with a task allocation and remains stable across calls. */
    static char boot_thread;
    return task ? (void *)task : (void *)&boot_thread;
}
uacpi_handle uacpi_kernel_create_spinlock(void) {
    spinlock_t *lock = kmalloc(sizeof(*lock));
    if (lock)
        spin_init(lock);
    return lock;
}
void uacpi_kernel_free_spinlock(uacpi_handle lock) {
    kfree(lock);
}
uacpi_cpu_flags uacpi_kernel_lock_spinlock(uacpi_handle lock) {
    return spin_lock_irqsave((spinlock_t *)lock);
}
void uacpi_kernel_unlock_spinlock(uacpi_handle lock, uacpi_cpu_flags flags) {
    spin_unlock_irqrestore((spinlock_t *)lock, flags);
}
uacpi_status
uacpi_kernel_handle_firmware_request(uacpi_firmware_request *request) {
    if (!request)
        return UACPI_STATUS_INVALID_ARGUMENT;
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_BREAKPOINT) {
        kwarn("[ACPI] AML breakpoint\n");
        return UACPI_STATUS_OK;
    }
    if (request->type == UACPI_FIRMWARE_REQUEST_TYPE_FATAL)
        kerr("[ACPI] AML Fatal type=%u code=%u arg=%llu\n", request->fatal.type,
             request->fatal.code, (unsigned long long)request->fatal.arg);
    return UACPI_STATUS_INTERNAL_ERROR;
}

typedef struct acpi_work {
    struct acpi_work *next;
    uacpi_work_handler handler;
    void *context;
    int queued;
} acpi_work_t;
static spinlock_t work_lock = SPINLOCK_INIT;
static acpi_work_t *work_head, **work_tail = &work_head;
static wait_queue_t work_waiters = WAIT_QUEUE_INIT;
static unsigned work_active;
static int worker_started;
/* Set by architecture SCI wrappers around each interpreter interrupt handler. */
static unsigned irq_active;
void acpi_os_interrupt_enter(void) {
    __atomic_add_fetch(&irq_active, 1, __ATOMIC_ACQUIRE);
}
void acpi_os_interrupt_leave(void) {
    __atomic_sub_fetch(&irq_active, 1, __ATOMIC_RELEASE);
}

uacpi_handle uacpi_kernel_create_work_item(void) {
    return kcalloc(1, sizeof(acpi_work_t));
}
void uacpi_kernel_free_work_item(uacpi_handle handle) {
    acpi_work_t *work = handle;
    if (!work)
        return;
    uint64_t flags = spin_lock_irqsave(&work_lock);
    int queued = work->queued;
    spin_unlock_irqrestore(&work_lock, flags);
    if (queued)
        panic("ACPI: freeing queued work");
    kfree(work);
}
void uacpi_kernel_schedule_work(uacpi_work_type type, uacpi_handle handle,
                                uacpi_work_handler handler,
                                uacpi_handle context) {
    (void)type; /* A single CPU0 worker also satisfies GPE CPU affinity. */
    acpi_work_t *work = handle;
    if (!work || !handler)
        panic("ACPI: invalid deferred work");
    uint64_t flags = spin_lock_irqsave(&work_lock);
    if (work->queued)
        panic("ACPI: duplicate deferred work");
    work->next = NULL;
    work->handler = handler;
    work->context = context;
    work->queued = 1;
    *work_tail = work;
    work_tail = &work->next;
    spin_unlock_irqrestore(&work_lock, flags);
    wait_queue_wake_one(&work_waiters, 0, PROC_WAKE_EVENT);
}
static int run_one_work(void) {
    uint64_t flags = spin_lock_irqsave(&work_lock);
    acpi_work_t *work = work_head;
    if (!work) {
        spin_unlock_irqrestore(&work_lock, flags);
        return 0;
    }
    work_head = work->next;
    if (!work_head)
        work_tail = &work_head;
    uacpi_work_handler handler = work->handler;
    void *context = work->context;
    work->queued = 0;
    work_active++;
    spin_unlock_irqrestore(&work_lock, flags);
    /* The callback may free or requeue its work item. Never touch it again. */
    handler(context);
    flags = spin_lock_irqsave(&work_lock);
    work_active--;
    spin_unlock_irqrestore(&work_lock, flags);
    return 1;
}
static void acpi_worker(void) {
    proc_set_name(proc_current(), "acpi");
    for (;;) {
        while (run_one_work()) {
        }
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_UNINTERRUPTIBLE, 0);
        wait_queue_entry_t entry = {0};
        uint64_t flags = spin_lock_irqsave(&work_lock);
        bool linked =
            !work_head && wait_queue_link(&work_waiters, &entry, token, 0);
        spin_unlock_irqrestore(&work_lock, flags);
        if (linked)
            proc_park_commit(token);
        else
            proc_park_cancel(token);
        wait_queue_unlink(&work_waiters, &entry);
        proc_park_finish(token);
    }
}
int acpi_os_start_worker(void) {
    if (worker_started)
        return 0;
    int pid = proc_alloc_deferred(acpi_worker);
    if (pid < 0)
        return pid;
    task_t *task = proc_find_get(pid);
    if (!task)
        return -ESRCH;
    proc_sched_config_t config;
    int rc = proc_sched_get(task, &config);
    if (!rc) {
        config.affinity = 1;
        rc = proc_sched_set(task, &config);
    }
    if (!rc) {
        worker_started = 1;
        proc_publish_deferred_task(task);
    } else
        proc_force_exit(task, 1);
    proc_put(task);
    return rc;
}
uacpi_status uacpi_kernel_wait_for_work_completion(void) {
    while (__atomic_load_n(&irq_active, __ATOMIC_ACQUIRE))
        cpu_relax();
    for (;;) {
        /* Prior to scheduler startup, the boot CPU drains the same queue. */
        if (!worker_started)
            while (run_one_work()) {
            }
        uint64_t flags = spin_lock_irqsave(&work_lock);
        int done = !work_head && !work_active;
        spin_unlock_irqrestore(&work_lock, flags);
        if (done)
            return UACPI_STATUS_OK;
        uacpi_kernel_sleep(1);
    }
}

#include "proc/proc_internal.h"
#include "core/cpu.h"
#include "core/panic.h"
#include "mm/vm.h"

/*
 * Current task storage.
 *
 * Each CPU publishes the task currently executing on its kernel stack.
 *
 * PER_CPU_CURRENT_VALIDATION:
 * - CPU 0 sets current during proc_init(); every secondary CPU must set its own
 *   slot exactly once before enabling preemption or taking scheduler IPIs.
 * - proc_set_current() is the only writer for g_cpu_current[cpu]. A context
 *   switch changes exactly the current CPU slot and never another CPU's slot.
 * - g_cpu_switching_out keeps the old stack owner visible until the replacement
 *   task calls proc_switch_complete(). That completion is also the only path
 *   which clears old->on_cpu and publishes a raced READY task to a runqueue.
 * - Cross-CPU wakeup and IPI reschedule tests must prove that cpu_current_id(),
 *   runqueue selection, and current slot lookup agree for the CPU handling the
 *   interrupt.
 *
 * LOCKING (see docs/roadmap/lock-serialization-split.md §1.2):
 * - The switching-out slot and the outgoing task's ownership release are
 *   serialized by the per-CPU g_cpu_switch_out[cpu].lock. It is strictly
 *   outside every task lock: g_cpu_switch_out[cpu] -> park_lock -> runq_lock.
 * - The outgoing task's ->on_cpu / ->owner_cpu are park_lock-owned (INV-P1), so
 *   the completion window holds exactly one task lock. The pending outgoing
 *   task is completed before proc_set_current() replaces the slot, which is
 *   what the old "complete the stale predecessor from inside proc_set_current"
 *   callback used to do under the global proc_lock.
 * - g_cpu_current[] remains acquire/release-published by its owning CPU;
 *   g_cpu_current_lock[] lets remote scheduler snapshots copy scalars before
 *   the slot-owned reference can be released.
 */
static task_t *g_cpu_current[CONFIG_NR_CPUS];
static task_t *g_cpu_switching_out[CONFIG_NR_CPUS];
static spinlock_t g_cpu_switch_out[CONFIG_NR_CPUS];
/* Protects current-slot replacement against remote scheduler snapshots. */
static spinlock_t g_cpu_current_lock[CONFIG_NR_CPUS];

void proc_current_slots_init(void)
{
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        spin_init(&g_cpu_switch_out[cpu]);
        spin_set_debug(&g_cpu_switch_out[cpu], "switch_out", NULL);
        spin_init(&g_cpu_current_lock[cpu]);
        spin_set_debug(&g_cpu_current_lock[cpu], "current_slot", NULL);
        g_cpu_current[cpu] = NULL;
        g_cpu_switching_out[cpu] = NULL;
    }
}

/*
 * Release the CPU slot held by the outgoing task and publish a raced READY
 * task to its runqueue.  Caller holds g_cpu_switch_out[cpu] and, once a
 * pending outgoing task exists, that task's park_lock.
 */
static task_t *proc_switch_complete_locked(unsigned cpu)
{
    task_t *old =
        __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE);
    if (!old)
        return 0;

#if CONFIG_DEBUG_SCHED_STATE
    if (!old->on_cpu || old->owner_cpu != cpu)
        panic("switch complete: pid=%d on_cpu=%d owner=%u cpu=%u",
              old->pid, old->on_cpu, old->owner_cpu, cpu);
#endif
    old->on_cpu = 0;
    old->owner_cpu = PROC_CPU_NONE;
    task_t *current = proc_current_on_cpu(cpu);
    if (old->mm && (!current || old->mm != current->mm))
        mm_context_leave(old->mm, cpu);

    /*
     * yield and Park/Wake may publish READY while the old task still owns this
     * CPU. It becomes queueable only after execution has continued on the
     * replacement stack.
     */
    if (old->state == PROC_READY &&
        !__atomic_load_n(&old->dispatching, __ATOMIC_RELAXED) &&
        !__atomic_load_n(&old->on_rq, __ATOMIC_RELAXED)) {
        unsigned target = cpu;
        if (old->affinity_migrate_pending) {
            target = proc_sched_select_cpu_locked(old);
            old->cpu_id = target;
            old->affinity_migrate_pending = 0;
        }
        proc_runq_enqueue_locked(old);
        if (old->on_rq && (target != cpu || proc_sched_should_preempt_locked(old, target)))
            proc_sched_request_cpu(target, 1);
    }
    proc_sched_assert_task_locked(old);

    /* Transfer a pin to the unlocked notifier before dropping CPU ownership.
     * Another CPU may reap immediately after the switching slot is cleared;
     * the pin keeps the task and its thread-group anchor alive meanwhile. */
    task_t *reap = old->state == PROC_ZOMBIE ? proc_get(old) : NULL;
    __atomic_store_n(&g_cpu_switching_out[cpu], NULL, __ATOMIC_RELEASE);
    /* Drop the CPU slot reference after the outgoing stack is inactive. */
    proc_put(old);
    return reap;
}

/*
 * Finish a pending outgoing task left behind by a previous switch on this CPU.
 * It is the A' half of the split: the outgoing work happens in its own
 * g_cpu_switch_out -> park_lock window, before the new current is published,
 * instead of as a callback under one global lock.
 */
void proc_switch_out_finish_pending(void)
{
    unsigned cpu = cpu_current_id();
    if (cpu >= CONFIG_NR_CPUS)
        return;

    uint64_t sf = spin_lock_irqsave(&g_cpu_switch_out[cpu]);
    task_t *pending =
        __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE);
    if (!pending) {
        spin_unlock_irqrestore(&g_cpu_switch_out[cpu], sf);
        return;
    }
    uint64_t pf = spin_lock_irqsave(&pending->park_lock);
    task_t *reap = proc_switch_complete_locked(cpu);
    spin_unlock_irqrestore(&pending->park_lock, pf);
    spin_unlock_irqrestore(&g_cpu_switch_out[cpu], sf);
    if (reap) {
        /* No scheduler/park lock may nest outside tasklist_lock. */
        proc_notify_reap_ready(reap);
        proc_sched_note_zombie();
        proc_put(reap);
    }
}

task_t *proc_current(void)
{
#if ARCH_FAST_CURRENT
    /*
     * This architecture keeps the task which owns the currently executing
     * kernel stack in a single register (tp on RISC-V).  Returning it directly avoids the former hot-path round trip
     * through arch_current_cpu_id() (tp -> cpu_id -> g_cpu_current[]) on
     * every syscall helper.  The per-CPU array remains the publication point
     * for remote observers and for proc_current_on_cpu().
     *
     * Early boot is the only interval in which tp need not contain a task;
     * retain the indexed fallback for that case.
     */
    task_t *current = (task_t *)arch_get_task_pointer();
    if (current && arch_is_kernel_address(current))
        return current;
#endif
#if CONFIG_NR_CPUS == 1
    /*
     * Single-CPU build: skip cpu_current_id() entirely.  On x86_64 that
     * helper executes CPUID, which is a KVM VM-exit (~microseconds) and this
     * is an extreme hot path -- readiness scans call proc_current() once per
     * polled fd, and the desktop workload polls constantly.  With one slot
     * there is nothing to index, so load g_cpu_current[0] directly.  The
     * slot is written only by proc_set_current() on this same CPU (RELEASE),
     * so an ACQUIRE load observes each published current task.
     */
    return __atomic_load_n(&g_cpu_current[0], __ATOMIC_ACQUIRE);
#else
    /* An IRQ may migrate this continuation after the CPU id is read but
     * before its slot is loaded.  Pin that pair of operations, otherwise
     * the old CPU's replacement task is returned as our current task. */
    uint64_t flags = arch_irqs_enabled();
    arch_local_irq_disable();
    task_t *result =
        __atomic_load_n(&g_cpu_current[cpu_current_id()], __ATOMIC_ACQUIRE);
    if (flags)
        arch_local_irq_enable();
    return result;
#endif
}

void proc_io_account(uint64_t rbytes, uint64_t wbytes,
                     uint64_t read_to_device, uint64_t write_to_device)
{
    task_t *t = proc_current();
    if (!t)
        return;
    if (rbytes) {
        __atomic_fetch_add(&t->io_rchar, rbytes, __ATOMIC_RELAXED);
        __atomic_fetch_add(&t->io_syscr, 1, __ATOMIC_RELAXED);
    }
    if (wbytes) {
        __atomic_fetch_add(&t->io_wchar, wbytes, __ATOMIC_RELAXED);
        __atomic_fetch_add(&t->io_syscw, 1, __ATOMIC_RELAXED);
    }
    if (read_to_device)
        __atomic_fetch_add(&t->io_read_bytes, read_to_device, __ATOMIC_RELAXED);
    if (write_to_device)
        __atomic_fetch_add(&t->io_write_bytes, write_to_device, __ATOMIC_RELAXED);
}

task_t *proc_set_current(task_t *next)
{
    unsigned cpu = cpu_current_id();
    if (!proc_get(next))
        panic("proc_set_current: cpu=%u next=%p refs=%d state=%d owner=%u",
              cpu, next, next ? refcount_read(&next->refs) : 0,
              next ? next->state : PROC_UNUSED,
              next ? next->owner_cpu : PROC_CPU_NONE);
    /*
     * Some architectures restore interrupt state in __switch before returning
     * to the C completion hook.  If that incoming task is immediately
     * preempted, the stale predecessor is finished by the caller through
     * proc_switch_out_finish_pending() before this slot is replaced -- doing it
     * here would nest a second task lock inside the caller's next->park_lock
     * window with no defined order between the two.
     */
    uint64_t flags = spin_lock_irqsave(&g_cpu_current_lock[cpu]);
    task_t *old = g_cpu_current[cpu];
    /* Publish the outgoing task before replacing current. Reapers must keep
     * its task storage and kernel stack alive until the switch completes. */
    __atomic_store_n(&g_cpu_switching_out[cpu], old, __ATOMIC_RELEASE);
    __atomic_store_n(&g_cpu_current[cpu], next, __ATOMIC_RELEASE);
    spin_unlock_irqrestore(&g_cpu_current_lock[cpu], flags);
    return old;
}

void proc_switch_complete(void)
{
    unsigned cpu = cpu_current_id();
    if (cpu >= CONFIG_NR_CPUS)
        return;

    uint64_t sf = spin_lock_irqsave(&g_cpu_switch_out[cpu]);
    task_t *old =
        __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE);
    if (!old) {
        spin_unlock_irqrestore(&g_cpu_switch_out[cpu], sf);
        return;
    }
    uint64_t pf = spin_lock_irqsave(&old->park_lock);
    task_t *reap = proc_switch_complete_locked(cpu);
    spin_unlock_irqrestore(&old->park_lock, pf);
    spin_unlock_irqrestore(&g_cpu_switch_out[cpu], sf);
    if (reap) {
        /* No scheduler/park lock may nest outside tasklist_lock. */
        proc_notify_reap_ready(reap);
        proc_sched_note_zombie();
        proc_put(reap);
    }
}

task_t *proc_current_on_cpu(unsigned cpu)
{
    if (cpu >= CONFIG_NR_CPUS)
        return NULL;
    return __atomic_load_n(&g_cpu_current[cpu], __ATOMIC_ACQUIRE);
}

/*
 * Copy scheduler scalars while current-slot ownership protects the object.
 * Remote callers must not dereference proc_current_on_cpu() after the slot can
 * be replaced: switch completion may release the last task reference. The
 * slot lock is released before proc_set_current() takes a task park_lock, so
 * this helper is also safe for callers that hold candidate->park_lock. The
 * deadline helper serializes its 64-bit value on 32-bit targets; remaining
 * scheduler fields are best-effort hints rather than a coherent snapshot of
 * every concurrently mutable task field.
 */
int proc_current_sched_snapshot(unsigned cpu, const task_t *candidate,
                                proc_current_sched_snapshot_t *out)
{
    if (!out)
        return 0;
    *out = (proc_current_sched_snapshot_t){0};
    if (cpu >= CONFIG_NR_CPUS)
        return 0;

    uint64_t flags = spin_lock_irqsave(&g_cpu_current_lock[cpu]);
    task_t *current = __atomic_load_n(&g_cpu_current[cpu], __ATOMIC_ACQUIRE);
    if (current) {
        out->valid = 1;
        out->idle = current->pid == 0;
        out->running = current->state == PROC_RUNNING;
        out->same_task = current == candidate;
        out->policy = (uint32_t)current->sched_policy;
        out->priority = current->priority;
        out->eevdf_deadline = proc_sched_deadline_snapshot(current);
        out->dl_deadline = current->sched_policy == SCHED_DEADLINE
                               ? current->dl_next_release
                               : 0;
        out->dl_throttled = current->sched_policy == SCHED_DEADLINE
                                ? current->dl_throttled
                                : 0;
    }
    spin_unlock_irqrestore(&g_cpu_current_lock[cpu], flags);
    return 1;
}

int proc_task_is_current_any_cpu(task_t *task)
{
    if (!task)
        return 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (proc_current_on_cpu(cpu) == task ||
            __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE) == task)
            return 1;
    }
    return 0;
}

unsigned proc_current_owner_memberships_locked(task_t *task)
{
    if (!task)
        return 0;
    unsigned memberships = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (__atomic_load_n(&g_cpu_current[cpu], __ATOMIC_ACQUIRE) == task)
            memberships++;
        if (__atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE) ==
            task)
            memberships++;
    }
    return memberships;
}

unsigned proc_current_slot_count_locked(void)
{
    unsigned count = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        if (__atomic_load_n(&g_cpu_current[cpu], __ATOMIC_ACQUIRE))
            count++;
        if (__atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE))
            count++;
    }
    return count;
}

unsigned proc_current_lifetime_violations_locked(void)
{
    unsigned violations = 0;
    for (unsigned cpu = 0; cpu < CONFIG_NR_CPUS; cpu++) {
        task_t *slots[2] = {
            __atomic_load_n(&g_cpu_current[cpu], __ATOMIC_ACQUIRE),
            __atomic_load_n(&g_cpu_switching_out[cpu], __ATOMIC_ACQUIRE),
        };
        for (unsigned slot = 0; slot < 2; slot++) {
            task_t *task = slots[slot];
            if (!task)
                continue;
            if (!task->on_cpu || task->owner_cpu != cpu ||
                refcount_read(&task->refs) <= 0)
                violations++;
            if (!task->dynamic_alloc && refcount_read(&task->refs) < 2)
                violations++;
            for (unsigned other_cpu = 0; other_cpu < CONFIG_NR_CPUS;
                 other_cpu++) {
                task_t *other_current =
                    __atomic_load_n(&g_cpu_current[other_cpu],
                                    __ATOMIC_ACQUIRE);
                task_t *other_switching =
                    __atomic_load_n(&g_cpu_switching_out[other_cpu],
                                    __ATOMIC_ACQUIRE);
                if (other_cpu < cpu && other_current == task)
                    violations++;
                if (other_cpu < cpu && other_switching == task)
                    violations++;
                if (other_cpu == cpu && slot == 1 &&
                    other_current == task)
                    violations++;
            }
        }
    }
    return violations;
}

int proc_task_pid(const void *task)
{
    const task_t *t = (const task_t *)task;
    return t ? t->pid : -1;
}

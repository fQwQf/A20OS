/*
 * MCU scheduler bring-up stubs.
 *
 * A20OS was written for rich MMU architectures.  The STM32 NOMMU build
 * deliberately cuts out BPF, futexes, cgroups, full VFS, page faults, and
 * the general kernel-progress engine.  These weak/empty symbols satisfy the
 * linker until the MCU port is fully trimmed or the real modules are wired in.
 */

#include "core/types.h"
#include "core/lock_counters.h"
#include "core/smp.h"
#include "core/timer.h"
#include "abi/native/ipc_internal.h"
#include "cg/cgroup.h"
#include "drivers/core/udriver.h"
#include "ext/kep.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/locks.h"
#include "fs/vfs.h"
#include "ipc/handle_table.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "proc/debug.h"
#include "proc/proc.h"
#include "sys/futex.h"

/* From kernel/proc/sched.c: event-driven network / block bottom halves. */
void kernel_progress_run_bottom_halves(void) { }

/* The MCU profile is deliberately single-core. */
uint32_t smp_online_cpu_mask(void) { return 1U; }
int smp_cpu_is_online(unsigned cpu) { return cpu == 0; }
void smp_send_reschedule(unsigned cpu) { (void)cpu; }

/* MCU tasks are kernel threads and never own a user address space. */
void mm_context_enter(mm_struct_t *mm, unsigned cpu) {
    (void)mm;
    (void)cpu;
}
void mm_context_leave(mm_struct_t *mm, unsigned cpu) {
    (void)mm;
    (void)cpu;
}

/* Keep the shared scheduler's normal 10 ms EEVDF base slice. */
int g_sched_base_slice_ms = 10;

/* From kernel/core/timekeeping.c: architecture-independent timer tick. */
void a20_timer_tick(void) { }

/* Diagnostics omitted from the size-constrained MCU image. */
uint64_t g_perf_sw_context_switches;
void psi_tick(void) { }
void a20_monitor_tick(void) { }
void lock_counters_register(spinlock_t *lock, const char *name) {
    (void)lock;
    (void)name;
}
void lock_counters_enable_callsite(spinlock_t *lock) { (void)lock; }

/* NOMMU MCU builds do not own physical pages, but inline frame helpers still
 * reference the allocator descriptor while compiling exit paths. */
pfa_t pfa;

/* From kernel/proc/exit.c / cgroup integration. */
void cg_detach_task(struct cg_node *cg, int pid) { (void)cg; (void)pid; }
void cg_mem_uncharge(struct cg_node *cg, size_t nr_pages) {
    (void)cg;
    (void)nr_pages;
}

/* From kernel/proc/exit.c: BPF program references. */

/* From kernel/proc/exit.c: robust futex list. */
void exit_robust_list(task_t *t) { (void)t; }

/* From kernel/proc/signal.c: user futex wake. */
int futex_wake_user(int *uaddr, int nr) {
    (void)uaddr;
    (void)nr;
    return 0;
}

/* From kernel/proc/exit.c: MM teardown for exec/exit. */
void mm_destroy(mm_struct_t *mm) { (void)mm; }

/* Rich-process subsystems are not present in the MCU profile. */
void udriver_task_cleanup(int pid) { (void)pid; }
void udisk_task_exit(int pid) { (void)pid; }
void a20_registry_task_exit(int pid) { (void)pid; }
void kep_release_process(int pid) { (void)pid; }
void a20_ht_put_ref(struct a20_ht_internal *ht) { (void)ht; }
void proc_debug_tracer_exiting(task_t *tracer) { (void)tracer; }
void keyring_release_task(task_t *task) { (void)task; }
void landlock_release_task(task_t *task) { (void)task; }
void acct_task_exit(task_t *task) { (void)task; }
int proc_debug_signal_stop(int sig) { (void)sig; return 0; }
int proc_debug_event_stop(int sig, int event, uint64_t msg) {
    (void)sig;
    (void)event;
    (void)msg;
    return 0;
}

/* MCU kernel threads never initialize a descriptor table. */
void fdtable_close_all(task_t *task) { (void)task; }
void vfs_release_process_locks(int pid) { (void)pid; }

/* From kernel/proc/exit.c: A20 event notifications. */
void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1) {
    (void)target_object;
    (void)target_type;
    (void)event_type;
    (void)data0;
    (void)data1;
}
void a20_eventq_on_object_destroy(void *object, uint16_t object_type) {
    (void)object;
    (void)object_type;
}

/* From kernel/fs/locks.c: advisory lock release. */
void fs_locks_release_file(vfile_t *vf, uintptr_t owner) {
    (void)vf;
    (void)owner;
}
void fs_locks_release_process(int pid) { (void)pid; }
void fs_locks_release_process_file(vfile_t *vf, int pid) {
    (void)vf;
    (void)pid;
}

/* From kernel/fs/vfs/path_resolution.c: file reference lookup / release. */
vfile_t *vfs_get_file_ref(int fd) { (void)fd; return NULL; }
void vfs_put_file_ref(int fd, vfile_t *vf) { (void)fd; (void)vf; }

/* From kernel/fs/vfs/vnode.c: vnode reference drop. */
void vnode_put(vnode_t *vp) { (void)vp; }

/* From kernel/fs/file.c: generic file close helpers. */
int file_close_prepare(int fd, vfile_t **closed) {
    (void)fd;
    if (closed)
        *closed = NULL;
    return 0;
}
void vfile_free(vfile_t *vf) { (void)vf; }

/* From kernel/fs/vfs.c: fd reference counting used by fdtable. */
int vfs_ref_fd(int fd) { (void)fd; return 0; }

/* 64-bit atomic helpers required by Cortex-M3 (no native 8-byte atomics). */
uint64_t __atomic_load_8(const volatile void *ptr, int memorder) {
    (void)memorder;
    const volatile uint64_t *p = (const volatile uint64_t *)ptr;
    uint64_t v;
    uint32_t flags = arch_irq_save();
    v = *p;
    arch_irq_restore(flags);
    return v;
}

void __atomic_store_8(volatile void *ptr, uint64_t value, int memorder) {
    (void)memorder;
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint32_t flags = arch_irq_save();
    *p = value;
    arch_irq_restore(flags);
}

uint64_t __atomic_fetch_add_8(volatile void *ptr, uint64_t value,
                              int memorder) {
    (void)memorder;
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint32_t flags = arch_irq_save();
    uint64_t old = *p;
    *p = old + value;
    arch_irq_restore(flags);
    return old;
}

uint64_t __atomic_exchange_8(volatile void *ptr, uint64_t value,
                             int memorder) {
    (void)memorder;
    volatile uint64_t *p = (volatile uint64_t *)ptr;
    uint32_t flags = arch_irq_save();
    uint64_t old = *p;
    *p = value;
    arch_irq_restore(flags);
    return old;
}

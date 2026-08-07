/*
 * MCU scheduler bring-up stubs.
 *
 * A20OS was written for rich MMU architectures.  The STM32 NOMMU build
 * deliberately cuts out BPF, futexes, cgroups, full VFS, page faults, and
 * the general kernel-progress engine.  These weak/empty symbols satisfy the
 * linker until the MCU port is fully trimmed or the real modules are wired in.
 */

#include "core/types.h"
#include "core/timer.h"
#include "abi/native/ipc_internal.h"
#include "cg/cgroup.h"
#include "fs/file.h"
#include "fs/locks.h"
#include "fs/vfs.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "proc/proc.h"
#include "sys/futex.h"

/* From kernel/proc/sched.c: event-driven network / block bottom halves. */
void kernel_progress_run_bottom_halves(void) { }

/* From kernel/core/timekeeping.c: architecture-independent timer tick. */
void a20_timer_tick(void) { }

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

/* From kernel/proc/exit.c: A20 event notifications. */
void a20_event_notify(void *target_object, uint16_t target_type,
                      uint32_t event_type, uint64_t data0, uint64_t data1) {
    (void)target_object;
    (void)target_type;
    (void)event_type;
    (void)data0;
    (void)data1;
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

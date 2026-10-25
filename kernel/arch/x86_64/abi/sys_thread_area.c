#include "core/arch.h"
#include "core/errno.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

int64_t sys_set_thread_area(void *ptr) {
    task_t *t = proc_current();
    if (!t || !t->trap_ctx)
        return -ESRCH;
    /* Publish the FS base in both the saved frame and live MSR. */
    TRAP_CTX_TP(t->trap_ctx) = (uint64_t)(uintptr_t)ptr;
    t->arch_tls_fs_base = (uint64_t)(uintptr_t)ptr;
    arch_write_user_fs_base((uint64_t)(uintptr_t)ptr);
    return 0;
}

int64_t sys_get_thread_area(void *ptr) {
    task_t *t = proc_current();
    if (!t || !t->trap_ctx)
        return -ESRCH;
    if (!ptr)
        return -EFAULT;
    /* x86_64 get_thread_area(2): report the FS-base TLS pointer.  On x86_64
     * the MSR is authoritative (the live trap frame slot is not refreshed);
     * elsewhere the frame field is the only copy.  Linux returns -EINVAL
     * when no descriptor is set; A20OS always has the base available. */
    uint64_t base = TRAP_CTX_TP(t->trap_ctx);
    base = arch_read_user_fs_base();
    if (copy_to_user(ptr, &base, sizeof(uint64_t)) < 0)
        return -EFAULT;
    return 0;
}

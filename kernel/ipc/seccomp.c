#include "ipc/seccomp.h"
#include "arch/syscall_hook.h"

#include "core/klog.h"
#include "core/lock.h"
#include "core/signal_defs.h"
#include "core/string.h"
#include "core/syscall_nr.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"
#include "sys/usercopy.h"

typedef struct {
    uint32_t nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
} seccomp_data_wire_t;

extern int a20_seccomp_filter_install(void **chain, const void *ufprog);
extern void a20_seccomp_filter_inherit(void **dst, void *src);
extern void a20_seccomp_filter_release(void **chain);
extern uint32_t a20_seccomp_filter_evaluate(const void *chain, const seccomp_data_wire_t *sd);

void *a20_seccomp_alloc(size_t size) { return kmalloc(size); }
void a20_seccomp_free(void *ptr) { kfree(ptr); }
int a20_seccomp_copy_from_user(void *dst, const void *src, size_t len) {
    return copy_from_user(dst, src, len);
}

static const uint32_t SECCOMP_ARCH_TOKEN =
    (sizeof(uintptr_t) == 8 ? 0x80000000U : 0) |
    (__BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__ ? 0x40000000U : 0) |
    (uint32_t)ARCH_ELF_MACHINE; /* AUDIT_ARCH_<ISA> */

#ifndef SYS_SECCOMP
#define SYS_SECCOMP 1
#endif

typedef struct {
    uintptr_t call_addr;
    int32_t syscall;
    uint32_t arch;
} sigsys_fields_t;

int64_t seccomp_gate(trap_context_t *ctx, uint64_t nr, int64_t *ret_out)
{
    task_t *t = proc_current();
    if (!t || (!t->seccomp_chain && t->seccomp_mode != SECCOMP_MODE_STRICT))
        return 0;

    uint64_t args[6] = {
        TRAP_CTX_ARG0(ctx), TRAP_CTX_ARG1(ctx), TRAP_CTX_ARG2(ctx),
        TRAP_CTX_ARG3(ctx), TRAP_CTX_ARG4(ctx), TRAP_CTX_ARG5(ctx),
    };
    uint32_t act = seccomp_evaluate(t, nr, TRAP_CTX_EPC(ctx), args);
    uint32_t data = act & SECCOMP_RET_DATA;

    switch (act & SECCOMP_RET_ACTION_FULL) {
    case SECCOMP_RET_ALLOW:
        return 0;
    case SECCOMP_RET_LOG:
        kdebug("[SECCOMP] pid=%d nr=%lu logged\n", t->pid,
               (unsigned long)nr);
        return 0;
    case SECCOMP_RET_ERRNO:
        *ret_out = -(int64_t)data;
        return 1;
    case SECCOMP_RET_KILL_PROCESS:
        proc_exit_group(-SIGSYS);
        *ret_out = -EINTR;
        return 1;
    case SECCOMP_RET_KILL_THREAD:
        proc_exit(-SIGSYS);
        *ret_out = -EINTR;
        return 1;
    case SECCOMP_RET_TRAP: {
        arch_siginfo_t si;
        memset(&si, 0, sizeof(si));
        si.si_signo = SIGSYS;
        si.si_code = SYS_SECCOMP;
        si.si_errno = (int)data;
        sigsys_fields_t f = {
            .call_addr = (uintptr_t)TRAP_CTX_EPC(ctx),
            .syscall = (int32_t)nr,
            .arch = SECCOMP_ARCH_TOKEN,
        };
        memcpy(si._sifields, &f, sizeof(f));
        if (signal_send_thread_info(t->pid, SIGSYS, &si, sizeof(si)) < 0)
            proc_exit_group(-SIGSYS);
        /* SIGSYS describes the trapped instruction, including the original
         * syscall/result register. The handler supplies the eventual result. */
        *ret_out = (int64_t)TRAP_CTX_RET(ctx);
        return 1;
    }
    case SECCOMP_RET_TRACE:
    case SECCOMP_RET_USER_NOTIF:
        /* No ptrace-stop notification channel is wired for TRACE and the
         * USER_NOTIF fd does not exist; Linux also degrades TRACE to
         * -ENOSYS when nobody listens. */
        *ret_out = -ENOSYS;
        return 1;
    default:
        proc_exit_group(-SIGSYS);
        *ret_out = -EINTR;
        return 1;
    }
}

void seccomp_inherit(task_t *child, const task_t *parent)
{
    child->seccomp_chain = NULL;
    child->seccomp_mode = SECCOMP_MODE_DISABLED;
    if (!parent || !parent->seccomp_chain)
        return;
    a20_seccomp_filter_inherit(&child->seccomp_chain, parent->seccomp_chain);
    child->seccomp_mode = parent->seccomp_mode;
}

void seccomp_release(task_t *t)
{
    a20_seccomp_filter_release(&t->seccomp_chain);
    t->seccomp_mode = SECCOMP_MODE_DISABLED;
}

int seccomp_get_mode(const task_t *t)
{
    return t ? (int)t->seccomp_mode : -ESRCH;
}

int seccomp_set_strict(task_t *t)
{
    if (!t)
        return -ESRCH;
    if (t->seccomp_mode == SECCOMP_MODE_FILTER && t->seccomp_chain)
        return -EACCES;
    t->seccomp_mode = SECCOMP_MODE_STRICT;
    return 0;
}

int seccomp_install_filter(task_t *t, const void *ufprog)
{
    if (!t)
        return -ESRCH;
    if (t->seccomp_mode == SECCOMP_MODE_STRICT)
        return -EINVAL;
    if (!t->policy.no_new_privs && !proc_has_cap(t, CAP_SYS_ADMIN))
        return -EACCES;
    int rc = a20_seccomp_filter_install(&t->seccomp_chain, ufprog);
    if (rc < 0)
        return rc;
    t->seccomp_mode = SECCOMP_MODE_FILTER;
    return 0;
}

uint32_t seccomp_evaluate(const task_t *t, uint64_t nr, uint64_t ip,
                          const uint64_t args[6])
{
    if (!t || !t->seccomp_chain) {
        if (t && t->seccomp_mode == SECCOMP_MODE_STRICT) {
            linux_syscall_args_t call = {.nr = nr};
            arch_syscall_adjust_args(&call);
            switch (call.nr) {
            case SYS_read: case SYS_write: case SYS_exit:
            case SYS_sigreturn:
                return SECCOMP_RET_ALLOW;
            default:
                return SECCOMP_RET_KILL_THREAD;
            }
        }
        return SECCOMP_RET_ALLOW;
    }

    seccomp_data_wire_t sd;
    memset(&sd, 0, sizeof(sd));
    sd.nr = (uint32_t)nr;
    sd.arch = SECCOMP_ARCH_TOKEN;
    sd.instruction_pointer = ip;
    for (int i = 0; i < 6; i++)
        sd.args[i] = args[i];
    return a20_seccomp_filter_evaluate(t->seccomp_chain, &sd);
}

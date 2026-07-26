/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * All 90 syscalls have real kernel API backing.
 * Design references (see docs/native-abi/):
 *   03-handle.md §4   — handle operation semantics
 *   04-memory.md §4   — vm_* semantics
 *   06-docs/native-abi/06-security.md §3 — operation-rights mapping
 *   05-ipc.md §2–3    — channel/eventq semantics
 */
#include "core/types.h"
#include "core/klog.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/version.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "core/random.h"
#include "trap_frame.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/xattr.h"
#include "net/socket.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                    uint16_t type, a20_rights_t rights,
                                    uint64_t expiry_tick, uint32_t remaining_ops,
                                    uint32_t temporal_flags, uint8_t security_label);
int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                   uint16_t expected_type, a20_rights_t required_rights,
                                   a20_handle_entry_t *out);
void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
struct a20_ht_internal *task_get_a20_ht(task_t *t);
uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);
extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);

/* NATIVE_DEBUG_LIMITED_CONTRACT: Debug (0x0900) — limited compatibility
 * implementations without full stop/resume/watchpoint behavior. */

/* ABI_CORE_API_CONTRACT: Native Phase 2 syscalls delegate to abi_core_proc_exec
 * and abi_core_proc_mmap where a Linux ABI equivalent exists. */

int copy_path_from_user(char *dst, const char *uptr, uint32_t len)
{
    if (!uptr) return -1;
    if (len > 0 && len < MAX_PATH_LEN) {
        if (copy_from_user(dst, uptr, len) < 0) return -1;
        dst[len] = '\0';
    } else {
        for (int i = 0; i < MAX_PATH_LEN - 1; i++) {
            if (copy_from_user(&dst[i], &uptr[i], 1) < 0) return -1;
            if (dst[i] == '\0') break;
        }
        dst[MAX_PATH_LEN - 1] = '\0';
    }
    return 0;
}

void resolve_path(const char *in, char *out)
{
    task_t *cur = proc_current();
    if (in[0] == '/') {
        strncpy(out, in, MAX_PATH_LEN);
    } else if (cur && cur->fs.cwd[0]) {
        size_t clen = strlen(cur->fs.cwd);
        memcpy(out, cur->fs.cwd, clen);
        out[clen] = '/';
        strncpy(out + clen + 1, in, MAX_PATH_LEN - clen - 1);
    } else {
        strncpy(out, in, MAX_PATH_LEN);
    }
}

/* subsystem syscall implementations moved to sys_native_*.c */

/* ===== Thread create — complex, Phase 2 ===== */

int64_t sys_a20_thread_create(const a20_syscall_args_t *args)
{
    a20_thread_create_args_t *uargs = (a20_thread_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_thread_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();

    /* CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_THREAD | CLONE_SIGHAND
     * to create a thread (shares address space, fd table, signal handlers) */
    uint64_t clone_flags = 0x00000100ULL /* CLONE_VM */ |
                           0x00000200ULL /* CLONE_FS */ |
                           0x00000400ULL /* CLONE_FILES */ |
                           0x00008000ULL /* CLONE_THREAD */ |
                           0x00000800ULL /* CLONE_SIGHAND */;

    int ctid = 0;
    int pid = proc_clone(clone_flags, kargs.stack_base, NULL,
                          kargs.tls_base, &ctid, 0);
    if (pid < 0) return -A20_ERR_NO_MEMORY;

    task_t *new_task = proc_find_get(pid);
    if (new_task && new_task->trap_ctx && kargs.entry) {
        trap_context_t *tc = new_task->trap_ctx;
        TRAP_CTX_SET_RET(tc, (uint64_t)kargs.entry);
        TRAP_CTX_ARG1(tc) = (uint64_t)kargs.arg;
        TRAP_CTX_SET_SP(tc, kargs.stack_base);
    }
    proc_put(new_task);

    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)pid, A20_OBJ_TASK,
                                    A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                                    A20_RIGHT_STAT | A20_RIGHT_CONTROL | A20_RIGHT_DUP |
                                    A20_RIGHT_TRANSFER);
    if (h < 0) return h;

    kargs.out_thread = (a20_handle_t)h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return h;
}

int64_t sys_a20_task_set_sched(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_sched_args_t *uargs = (a20_sched_args_t *)A20_ARG(1);
    if (!uargs) return -A20_ERR_FAULT;

    a20_sched_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (!target) return -A20_ERR_BAD_HANDLE;

    proc_sched_config_t config = {
        .fields = kargs.flags,
        .policy = kargs.policy,
        .priority = kargs.priority,
        .nice = kargs.nice,
        .affinity = (uint32_t)kargs.affinity,
    };
    if ((kargs.flags & A20_SCHED_AFFINITY) &&
        (kargs.affinity_size < sizeof(uint64_t) || (kargs.affinity >> 32))) {
        proc_put(target);
        return -A20_ERR_INVALID_ARGUMENT;
    }
    int64_t result = proc_sched_set(target, &config) < 0
                         ? -A20_ERR_INVALID_ARGUMENT : A20_OK;
    proc_put(target);
    return result;
}

int64_t sys_a20_task_get_limits(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    struct a20_resource_limits *out = (struct a20_resource_limits *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_STAT, &entry);
    if (r < 0) return r;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    struct a20_resource_limits limits;
    a20_resource_limits_init_default(&limits);
    if (target) {
        limits.max_memory_bytes = (uint64_t)target->limits.stack;
    }
    proc_put(target);

    if (copy_to_user(out, &limits, sizeof(limits)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_task_set_limits(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    struct a20_resource_limits *uargs = (struct a20_resource_limits *)A20_ARG(1);
    if (!uargs) return -A20_ERR_FAULT;

    struct a20_resource_limits kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    if (kargs.max_handles > A20_LIMIT_HANDLES_ABSOLUTE)
        return -A20_ERR_ACCESS;
    if (kargs.max_channels > A20_LIMIT_CHANNELS_ABSOLUTE)
        return -A20_ERR_ACCESS;
    if (kargs.max_threads > A20_LIMIT_THREADS_ABSOLUTE)
        return -A20_ERR_ACCESS;
    if (kargs.max_memory_bytes > A20_LIMIT_MEMORY_ABSOLUTE)
        return -A20_ERR_ACCESS;

    task_t *target = proc_find_get((int)(uintptr_t)entry.object);
    if (!target) return -A20_ERR_BAD_HANDLE;
    if (target->limits.nofile == 0 || kargs.max_handles < target->limits.nofile)
        target->limits.nofile = (uint32_t)kargs.max_handles;

    proc_put(target);
    return A20_OK;
}

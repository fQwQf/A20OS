#ifndef A20_NATIVE_RUNTIME_SHIM
#define A20_NATIVE_RUNTIME_SHIM
/* Hosted task/capability/usercopy/table adapters; the runtime core and syscall
 * bodies are compiled unchanged. Real syscall entry is tested in QEMU/MCU. */
#include "native_task_object_shim.h"
#define _ABI_NATIVE_SYSCALL_ENTRY_H
typedef struct a20_syscall_args {
    uint64_t nr, arg[6];
    void *ctx;
} a20_syscall_args_t;
#define CAP_SYS_ADMIN 21
task_t *proc_current(void);
int proc_has_cap(const task_t *task, int cap);
struct a20_ht_internal *task_get_a20_ht(task_t *task);
int64_t sys_a20_runtime_info(const a20_syscall_args_t *args);
int64_t sys_a20_runtime_bind(const a20_syscall_args_t *args);
int64_t sys_a20_handle_identity(const a20_syscall_args_t *args);
#endif

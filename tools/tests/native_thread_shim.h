#ifndef A20_NATIVE_THREAD_TEST_SHIM
#define A20_NATIVE_THREAD_TEST_SHIM
#include <stdint.h>
#include <stddef.h>
#include "core/types.h"
#include "core/errno.h"
/* Hosted dependency adapter. Compile the production ABI admission handler;
 * fail allocation/handle installation/copyout deterministically. */
#define _DEFS_H
#define A20_TASK_WAIT_NONBLOCK 1u
#define _PROC_H
#define _IPC_HANDLE_TABLE_H
#define _IPC_IPC_H
#define _TRAP_H
#define _ABI_NATIVE_TYPES_H
#include "../../user/liba20rt/a20_types.h"
typedef struct task_t {
    int pid;
    unsigned a20_joinable;
} task_t;
typedef struct trap_context {
    unsigned unused;
} trap_context_t;
struct a20_ht_internal {
    unsigned unused;
};
task_t *proc_current(void);
task_t *proc_find_get(int pid);
void proc_put(task_t *task);
void proc_free_pid(int pid);
void proc_publish_deferred_task(task_t *task);
int proc_wait4(int pid, int *status, int flags);
int proc_create_thread_deferred(uint64_t entry, uint64_t arg, vaddr_t sp,
                                vaddr_t tls, size_t size, task_t **out);
struct a20_ht_internal *task_get_a20_ht(task_t *task);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t handle);
int64_t a20_handle_lookup_task_like(struct a20_ht_internal *ht,
                                    a20_handle_t handle, a20_rights_t rights,
                                    a20_handle_entry_t *out);
#endif

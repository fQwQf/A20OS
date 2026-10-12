#ifndef A20_NATIVE_SPAWN_TEST_SHIM
#define A20_NATIVE_SPAWN_TEST_SHIM
#include <stdint.h>
#include <stddef.h>
#include "core/types.h"
#include "core/errno.h"
#define spin_lock_irqsave(lock) ((void)(lock), 0ULL)
#define spin_unlock_irqrestore(lock, flags) ((void)(lock), (void)(flags))
#define _DEFS_H
#define _PROC_H
#define _IPC_HANDLE_TABLE_H
#define _IPC_IPC_H
#define _IPC_START_INFO_H
#define _TRAP_H
#define _ABI_NATIVE_TYPES_H
#define A20_FLAT_FILES_H
#define A20_TASK_SPAWN_ARGS_V1_SIZE 72u
#include "../../user/liba20rt/a20_types.h"
typedef struct task_t {
    int pid, pdeathsig;
    unsigned park_lock;
} task_t;
typedef struct trap_context {
    unsigned unused;
} trap_context_t;
typedef struct vfile {
    unsigned unused;
} vfile_t;
struct a20_ht_internal {
    unsigned unused;
};
task_t *proc_current(void);
void proc_put(task_t *);
void proc_free_pid(int);
void proc_publish_deferred_task(task_t *);
struct a20_ht_internal *task_get_a20_ht(task_t *);
int64_t a20_handle_install_task(struct a20_ht_internal *, task_t *, uint16_t,
                                a20_rights_t);
int64_t a20_handle_remove(struct a20_ht_internal *, a20_handle_t);
int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *, a20_handle_t,
                                       uint16_t, a20_rights_t,
                                       a20_handle_entry_t *);
void a20_object_release(void *, uint16_t);
int a20_flat_file_image(vfile_t *, size_t, void **, size_t *);
#endif

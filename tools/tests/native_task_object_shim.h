#ifndef A20_NATIVE_TASK_OBJECT_SHIM
#define A20_NATIVE_TASK_OBJECT_SHIM
/* Hosted hardware/task/table adapters. Identity, PID allocator and generic
 * object ref/release bodies are compiled from production sources unchanged. */
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <sched.h>
#include "core/types.h"
#include "core/refcount.h"
#include "core/errno.h"
#include "../../user/liba20rt/a20_types.h"
#define _PANIC_H
__attribute__((noreturn)) void panic(const char *, ...);
#define _PROC_H
#define _PROC_INTERNAL_H
#define _PROC_LIFETIME_H
#define _CORE_LOCK_H
#define _IPC_HANDLE_TABLE_H
#define _IPC_IPC_H
#define _MM_VMAR_H
#define _SLAB_H
#define _VFS_H
#define _MM_VMO_H
#define _EXT_KEP_H
#define A20_TASK_OBJECTS_ENABLED 1
#define CONFIG_MCU 1
#define CONFIG_MCU_NATIVE_PROGRAM 1
#define CONFIG_NATIVE_TASK_HANDLES_ONLY 1
struct vfile;
struct a20_task_object;
typedef struct task_t {
    refcount_t refs;
    int pid, destroy_started;
    proc_state_t state;
    struct task_t *pid_hash_next;
    struct a20_task_object *a20_object;
} task_t;
typedef struct spinlock {
    int held;
} spinlock_t;
#define SPINLOCK_INIT {0}
static inline void spin_init(spinlock_t *lock) {
    lock->held = 0;
}
static inline uint64_t spin_lock_irqsave(spinlock_t *lock) {
    while (__atomic_exchange_n(&lock->held, 1, __ATOMIC_ACQUIRE))
        sched_yield();
    return 0;
}
static inline void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    __atomic_store_n(&lock->held, 0, __ATOMIC_RELEASE);
}
void *kcalloc(size_t count, size_t size);
void kfree(void *object);
task_t *proc_get(task_t *task);
void proc_put(task_t *task);
void proc_pid_init(void);
int proc_pid_alloc(void);
void proc_pid_register(task_t *task);
void proc_pid_unregister(task_t *task);
int proc_set_pid_max(int value);
task_t *proc_find_get(int pid);
static inline void proc_lifetime_note_pid_add(void) {
}
static inline void proc_lifetime_note_pid_remove(void) {
}
struct a20_ht_internal;
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                       a20_handle_t handle, uint16_t type,
                                       a20_rights_t rights,
                                       a20_handle_entry_t *out);
int64_t a20_handle_lookup_ref_task_like(struct a20_ht_internal *ht,
                                        a20_handle_t handle,
                                        a20_rights_t rights,
                                        a20_handle_entry_t *out);
int64_t a20_handle_install_task(struct a20_ht_internal *ht, task_t *task,
                                uint16_t type, a20_rights_t rights);
int64_t a20_handle_task_get(struct a20_ht_internal *ht, a20_handle_t handle,
                            uint16_t type, a20_rights_t rights, task_t **out);
void a20_object_ref(void *object, uint16_t type);
void a20_object_release(void *object, uint16_t type);
void a20_event_notify(void *object, uint16_t type, uint32_t event,
                      uint64_t data0, uint64_t data1);
void a20_eventq_on_object_destroy(void *object, uint16_t type);
#endif

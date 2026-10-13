/* Native task identity is independent of PID reuse and of task storage.
 * Handles and in-flight channel messages own this small object, never the
 * task/address space. The task owns one ref until its final release; the
 * binding is weak and cleared before PID unregistration. */
#include "proc/task_object.h"
#ifdef A20_TASK_OBJECTS_ENABLED
#include "core/lock.h"
#include "core/refcount.h"
#include "mm/slab.h"
#include "ipc/handle_table.h"

struct a20_task_object {
    refcount_t refs;
    task_t *task;
    uint64_t serial;
};
/* Shared by the C reference and Rust implementation. */
static spinlock_t task_object_lock = SPINLOCK_INIT;
#ifdef CONFIG_TASK_OBJECT_RUST
#include <stddef.h>
_Static_assert(sizeof(refcount_t) == 4, "task object ref ABI");
_Static_assert(offsetof(struct a20_task_object, refs) == 0,
               "task object refs ABI");
_Static_assert(offsetof(struct a20_task_object, task) ==
                   (sizeof(void *) == 8 ? 8 : 4),
               "task object task ABI");
_Static_assert(offsetof(struct a20_task_object, serial) ==
                   (sizeof(void *) == 8 ? 16 : 8),
               "task object serial ABI");
_Static_assert(sizeof(struct a20_task_object) ==
                   (sizeof(void *) == 8 ? 24 : 16),
               "task object size ABI");

/* Rust owns identity publication, references, detach and final release. These
 * adapters keep the architecture-specific task layout and irq-save lock in C. */
extern uint64_t a20_rust_task_object_serial(void *object);
extern void a20_rust_task_object_ref(void *object);
extern void a20_rust_task_object_put(void *object);
extern task_t *a20_rust_task_object_get_task(void *object);
extern void a20_rust_task_object_detach(task_t *task);
extern void a20_rust_task_object_task_release(task_t *task);
extern int64_t a20_rust_handle_install_task(struct a20_ht_internal *ht,
                                            task_t *task, uint16_t type,
                                            a20_rights_t rights);
extern void a20_rust_task_object_notify(task_t *task, uint32_t event,
                                        uint64_t data0, uint64_t data1,
                                        int closing);

uint64_t a20_task_object_lock(void) {
    return spin_lock_irqsave(&task_object_lock);
}
void a20_task_object_unlock(uint64_t flags) {
    spin_unlock_irqrestore(&task_object_lock, flags);
}
/* Caller holds task_object_lock after reading the weak task pointer from the
 * Rust-owned identity. Keep the task-layout acquire check and proc_get together
 * so destruction cannot reclaim storage between them. This function neither
 * locks nor calls callbacks; the caller releases the returned pin with
 * proc_put after leaving the identity critical section. */
task_t *a20_task_object_try_pin_task(task_t *task) {
    if (!task || __atomic_load_n(&task->destroy_started, __ATOMIC_ACQUIRE))
        return NULL;
    return proc_get(task);
}
void *a20_task_object_slot_get(task_t *task) {
    return task ? task->a20_object : NULL;
}
void a20_task_object_slot_set(task_t *task, void *object) {
    task->a20_object = object;
}
int a20_task_object_destroy_started(task_t *task) {
    return task ? __atomic_load_n(&task->destroy_started, __ATOMIC_ACQUIRE) : 1;
}

uint64_t a20_task_object_serial(void *object) {
    return a20_rust_task_object_serial(object);
}
void a20_task_object_ref(void *object) {
    a20_rust_task_object_ref(object);
}
void a20_task_object_put(void *object) {
    a20_rust_task_object_put(object);
}
task_t *a20_task_object_get_task(void *object) {
    return a20_rust_task_object_get_task(object);
}
void a20_task_object_detach(task_t *task) {
    a20_rust_task_object_detach(task);
}
void a20_task_object_task_release(task_t *task) {
    a20_rust_task_object_task_release(task);
}
int64_t a20_handle_install_task(struct a20_ht_internal *ht, task_t *task,
                                uint16_t type, a20_rights_t rights) {
    return a20_rust_handle_install_task(ht, task, type, rights);
}
void a20_task_object_notify(task_t *task, uint32_t event, uint64_t data0,
                            uint64_t data1, int closing) {
    a20_rust_task_object_notify(task, event, data0, data1, closing);
}
#else
/* Serializes lazy publication, weak-to-strong acquisition and detach.
 * No allocator, table lock, park lock or task release under this lock. */
static uint64_t next_serial;

uint64_t a20_task_object_serial(void *object) {
    return object ? ((struct a20_task_object *)object)->serial : 0;
}

void a20_task_object_ref(void *object) {
    if (object)
        refcount_inc(&((struct a20_task_object *)object)->refs);
}
void a20_task_object_put(void *object) {
    if (object &&
        refcount_dec_and_test(&((struct a20_task_object *)object)->refs)) {
        /* Remove watches registered after exit before address reuse. */
        a20_eventq_on_object_destroy(object, A20_OBJ_TASK);
        a20_eventq_on_object_destroy(object, A20_OBJ_THREAD);
        a20_eventq_on_object_destroy(object, A20_OBJ_DEBUG);
        kfree(object);
    }
}
task_t *a20_task_object_get_task(void *object) {
    if (!object)
        return NULL;
    uint64_t flags = spin_lock_irqsave(&task_object_lock);
    task_t *task = ((struct a20_task_object *)object)->task;
    /* Logical destruction precedes weak detach. Reject the short interval
     * too, so poll/query never treats an UNUSED task as live. */
    if (task && __atomic_load_n(&task->destroy_started, __ATOMIC_ACQUIRE))
        task = NULL;
    task = proc_get(task);
    spin_unlock_irqrestore(&task_object_lock, flags);
    return task;
}
void a20_task_object_detach(task_t *task) {
    uint64_t flags = spin_lock_irqsave(&task_object_lock);
    if (task->a20_object)
        task->a20_object->task = NULL;
    spin_unlock_irqrestore(&task_object_lock, flags);
}
void a20_task_object_task_release(task_t *task) {
    a20_task_object_put(task->a20_object);
}

int64_t a20_handle_install_task(struct a20_ht_internal *ht, task_t *task,
                                uint16_t type, a20_rights_t rights) {
    if (!task || (type != A20_OBJ_TASK && type != A20_OBJ_THREAD &&
                  type != A20_OBJ_DEBUG))
        return -A20_ERR_BAD_HANDLE;
    /* Fast existing-object path avoids an allocation on every duplicate or
     * self handle. The caller already holds a live task reference. */
    struct a20_task_object *candidate = NULL, *object;
    for (;;) {
        uint64_t flags = spin_lock_irqsave(&task_object_lock);
        object = task->a20_object;
        if (__atomic_load_n(&task->destroy_started, __ATOMIC_ACQUIRE)) {
            spin_unlock_irqrestore(&task_object_lock, flags);
            a20_task_object_put(candidate);
            return -A20_ERR_BAD_HANDLE;
        }
        if (!object && candidate) {
            if (next_serial == UINT64_MAX) {
                spin_unlock_irqrestore(&task_object_lock, flags);
                a20_task_object_put(candidate);
                return -A20_ERR_NO_SPACE;
            }
            candidate->serial = ++next_serial;
            object = candidate;
            candidate = NULL;
            object->task = task;
            task->a20_object = object; /* initial ref belongs to task */
        }
        if (object)
            a20_task_object_ref(object); /* prospective handle ref */
        spin_unlock_irqrestore(&task_object_lock, flags);
        if (object)
            break;
        candidate = kcalloc(1, sizeof(*candidate));
        if (!candidate)
            return -A20_ERR_NO_MEMORY;
        refcount_set(&candidate->refs, 1);
    }
    a20_task_object_put(candidate);
    int64_t result = a20_handle_install(ht, object, type, rights);
    if (result < 0)
        a20_task_object_put(object);
    return result;
}

void a20_task_object_notify(task_t *task, uint32_t event, uint64_t data0,
                            uint64_t data1, int closing) {
    uint64_t flags = spin_lock_irqsave(&task_object_lock);
    struct a20_task_object *object = task->a20_object;
    a20_task_object_ref(object);
    spin_unlock_irqrestore(&task_object_lock, flags);
    if (!object)
        return;
    const uint16_t types[] = {A20_OBJ_TASK, A20_OBJ_THREAD, A20_OBJ_DEBUG};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        a20_event_notify(object, types[i], event, data0, data1);
        if (closing)
            a20_eventq_on_object_destroy(object, types[i]);
    }
    a20_task_object_put(object);
}
#endif

/* Handle decoding remains in C; the weak-to-strong identity pin above is
 * implemented by the selected lifecycle implementation. */
int64_t a20_handle_task_get(struct a20_ht_internal *ht, a20_handle_t handle,
                            uint16_t type, a20_rights_t rights, task_t **out) {
    *out = NULL;
    if (type != A20_OBJ_INVALID && type != A20_OBJ_TASK &&
        type != A20_OBJ_THREAD && type != A20_OBJ_DEBUG)
        return -A20_ERR_BAD_HANDLE;
    a20_handle_entry_t entry;
    int64_t result =
        type == A20_OBJ_INVALID
            ? a20_handle_lookup_ref_task_like(ht, handle, rights, &entry)
            : a20_handle_lookup_ref_internal(ht, handle, type, rights, &entry);
    if (result < 0)
        return result;
    *out = a20_task_object_get_task(entry.object);
    a20_object_release(entry.object, entry.type);
    return *out ? A20_OK : -A20_ERR_BAD_HANDLE;
}
#endif

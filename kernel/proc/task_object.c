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
/* Serializes lazy publication, weak-to-strong acquisition and detach.
 * No allocator, table lock, park lock or task release under this lock. */
static spinlock_t task_object_lock = SPINLOCK_INIT;
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

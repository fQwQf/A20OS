#include "native_task_object_shim.h"
#include "proc/task_object.h"
#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#define SLOTS 64
struct a20_ht_internal {
    spinlock_t lock;
    a20_handle_entry_t entries[SLOTS];
};
static struct a20_ht_internal table;
static int allocations, fail_alloc, fail_install, notifications, destroyed_keys;
static void *last_key;
static void *notification_keys[16];
static task_t *reenter_task;
static int reenter_on_destroy;
void *kcalloc(size_t n, size_t size) {
    if (__atomic_exchange_n(&fail_alloc, 0, __ATOMIC_RELAXED))
        return NULL;
    void *p = calloc(n, size);
    if (p)
        __atomic_add_fetch(&allocations, 1, __ATOMIC_RELAXED);
    return p;
}
void kfree(void *p) {
    if (p)
        __atomic_sub_fetch(&allocations, 1, __ATOMIC_RELAXED);
    free(p);
}
task_t *proc_get(task_t *task) {
    return task && refcount_inc_not_zero(&task->refs) ? task : NULL;
}
void proc_put(task_t *task) {
    if (!task || !refcount_dec_and_test(&task->refs))
        return;
    assert(task->destroy_started);
    a20_task_object_task_release(task);
    proc_pid_final_release(task);
    kfree(task);
}
static task_t *new_task(void) {
    task_t *task = kcalloc(1, sizeof(*task));
    assert(task);
    refcount_set(&task->refs, 1);
    task->pid = proc_pid_alloc();
    assert(task->pid > 0);
    task->state = PROC_READY;
    proc_pid_register(task);
    return task;
}
static void destroy(task_t *task) {
    __atomic_store_n(&task->destroy_started, 1, __ATOMIC_RELEASE);
    task->state = PROC_UNUSED;
    a20_task_object_detach(task);
    proc_pid_unregister(task);
    proc_put(task);
}
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights) {
    if (__atomic_exchange_n(&fail_install, 0, __ATOMIC_RELAXED))
        return -A20_ERR_NO_SPACE;
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    for (unsigned i = 0; i < SLOTS; ++i)
        if (!ht->entries[i].object) {
            ht->entries[i] = (a20_handle_entry_t){
                .object = object, .type = type, .rights = rights};
            spin_unlock_irqrestore(&ht->lock, flags);
            return i;
        }
    spin_unlock_irqrestore(&ht->lock, flags);
    return -A20_ERR_NO_SPACE;
}
int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                       a20_handle_t h, uint16_t type,
                                       a20_rights_t rights,
                                       a20_handle_entry_t *out) {
    uint64_t flags = spin_lock_irqsave(&ht->lock);
    int64_t result = -A20_ERR_BAD_HANDLE;
    if (h < SLOTS && ht->entries[h].object &&
        (type == A20_OBJ_INVALID || ht->entries[h].type == type)) {
        if ((ht->entries[h].rights & rights) != rights)
            result = -A20_ERR_ACCESS;
        else {
            *out = ht->entries[h];
            a20_object_ref(out->object, out->type);
            result = 0;
        }
    }
    spin_unlock_irqrestore(&ht->lock, flags);
    return result;
}
int64_t a20_handle_lookup_ref_task_like(struct a20_ht_internal *ht,
                                        a20_handle_t h, a20_rights_t rights,
                                        a20_handle_entry_t *out) {
    int64_t result =
        a20_handle_lookup_ref_internal(ht, h, A20_OBJ_TASK, rights, out);
    return result == -A20_ERR_BAD_HANDLE
               ? a20_handle_lookup_ref_internal(ht, h, A20_OBJ_THREAD, rights,
                                                out)
               : result;
}
static void close_handle(a20_handle_t h) {
    uint64_t flags = spin_lock_irqsave(&table.lock);
    assert(h < SLOTS && table.entries[h].object);
    a20_handle_entry_t entry = table.entries[h];
    table.entries[h].object = NULL;
    spin_unlock_irqrestore(&table.lock, flags);
    a20_object_release(entry.object, entry.type);
}
void a20_event_notify(void *object, uint16_t type, uint32_t event, uint64_t d0,
                      uint64_t d1) {
    assert(object && type && event == A20_EVENT_EXITED && d0 == 37 && !d1);
    last_key = object;
    if (notifications < (int)(sizeof(notification_keys) /
                              sizeof(notification_keys[0])))
        notification_keys[notifications] = object;
    notifications++;
}
void a20_eventq_on_object_destroy(void *object, uint16_t type) {
    assert(object && (type == A20_OBJ_TASK || type == A20_OBJ_THREAD ||
                      type == A20_OBJ_DEBUG));
    __atomic_add_fetch(&destroyed_keys, 1, __ATOMIC_RELAXED);
    /* This callback re-enters identity lookup/notify through a different live
     * object. It must run after task_object_lock has been released. */
    if (__atomic_exchange_n(&reenter_on_destroy, 0, __ATOMIC_ACQ_REL))
        a20_task_object_notify(reenter_task, A20_EVENT_EXITED, 37, 0, 0);
}
static void *race(void *arg) {
    task_t *task = arg;
    for (int i = 0; i < 2000; ++i) {
        int64_t h = a20_handle_install_task(&table, task, A20_OBJ_THREAD,
                                            A20_RIGHT_WAIT);
        if (h == -A20_ERR_BAD_HANDLE)
            break;
        assert(h >= 0);
        task_t *pin = NULL;
        int64_t result = a20_handle_task_get(&table, h, A20_OBJ_INVALID,
                                             A20_RIGHT_WAIT, &pin);
        assert(result == 0 || result == -A20_ERR_BAD_HANDLE);
        proc_put(pin);
        close_handle(h);
    }
    proc_put(task);
    return NULL;
}
int main(void) {
    proc_pid_init();
    assert(proc_set_pid_max(2) == 0);
    task_t *task = new_task();
    fail_alloc = 1;
    assert(a20_handle_install_task(&table, task, A20_OBJ_TASK,
                                   A20_RIGHT_WAIT) == -A20_ERR_NO_MEMORY);
    assert(!task->a20_object);
    fail_install = 1;
    assert(a20_handle_install_task(&table, task, A20_OBJ_TASK,
                                   A20_RIGHT_WAIT) == -A20_ERR_NO_SPACE);
    assert(task->a20_object && allocations == 2);
    int64_t h =
        a20_handle_install_task(&table, task, A20_OBJ_THREAD, A20_RIGHT_WAIT);
    int64_t dbg =
        a20_handle_install_task(&table, task, A20_OBJ_DEBUG, A20_RIGHT_READ);
    assert(h >= 0 && dbg >= 0);
    task_t *pin = NULL;
    assert(a20_handle_task_get(&table, h, A20_OBJ_FILE, A20_RIGHT_WAIT, &pin) ==
               -A20_ERR_BAD_HANDLE &&
           !pin);
    assert(a20_handle_task_get(&table, h, A20_OBJ_INVALID, A20_RIGHT_SIGNAL,
                               &pin) == -A20_ERR_ACCESS &&
           !pin);
    assert(a20_handle_task_get(&table, dbg, A20_OBJ_INVALID, A20_RIGHT_READ,
                               &pin) == -A20_ERR_BAD_HANDLE &&
           !pin);
    assert(a20_handle_task_get(&table, dbg, A20_OBJ_DEBUG, A20_RIGHT_READ,
                               &pin) == 0 &&
           pin == task);
    a20_handle_entry_t flight;
    assert(a20_handle_lookup_ref_task_like(&table, h, A20_RIGHT_WAIT,
                                           &flight) == 0);
    task_t callback_task = {0};
    refcount_set(&callback_task.refs, 1);
    int64_t callback_handle = a20_handle_install_task(
        &table, &callback_task, A20_OBJ_TASK, A20_RIGHT_WAIT);
    assert(callback_handle >= 0);
    reenter_task = &callback_task;
    reenter_on_destroy = 1;
    a20_task_object_notify(task, A20_EVENT_EXITED, 37, 0, 1);
    assert(notifications == 6 && notification_keys[0] == flight.object &&
           notification_keys[1] == callback_task.a20_object &&
           notification_keys[2] == callback_task.a20_object &&
           notification_keys[3] == callback_task.a20_object &&
           notification_keys[4] == flight.object &&
           notification_keys[5] == flight.object);
    reenter_task = NULL;
    /* Production marks destruction under task locks before weak detach. */
    __atomic_store_n(&task->destroy_started, 1, __ATOMIC_RELEASE);
    assert(!a20_task_object_get_task(flight.object));
    assert(a20_handle_install_task(&table, task, A20_OBJ_TASK,
                                   A20_RIGHT_WAIT) == -A20_ERR_BAD_HANDLE);
    destroy(task);
    assert(proc_pid_alloc() ==
           -EAGAIN); /* a PID-based operation still pins old task */
    assert(!proc_find_get(2));
    proc_put(pin);
    task_t *replacement = new_task();
    assert(replacement->pid == 2);
    int64_t replacement_handle = a20_handle_install_task(
        &table, replacement, A20_OBJ_TASK, A20_RIGHT_WAIT);
    assert(replacement_handle >= 0 &&
           a20_task_object_serial(flight.object) > 0);
    assert(a20_task_object_serial(replacement->a20_object) >
           a20_task_object_serial(flight.object));
    close_handle(replacement_handle);
    assert(a20_handle_task_get(&table, h, A20_OBJ_INVALID, A20_RIGHT_WAIT,
                               &pin) == -A20_ERR_BAD_HANDLE &&
           !pin);
    assert(a20_handle_task_get(&table, dbg, A20_OBJ_DEBUG, A20_RIGHT_READ,
                               &pin) == -A20_ERR_BAD_HANDLE &&
           !pin);
    assert(!a20_task_object_get_task(flight.object));
    /* Deliver an in-flight object after origin reap and PID reuse. */
    int64_t delivered =
        a20_handle_install(&table, flight.object, flight.type, flight.rights);
    assert(delivered >= 0);
    close_handle(h);
    close_handle(dbg);
    assert(a20_handle_task_get(&table, delivered, A20_OBJ_INVALID,
                               A20_RIGHT_WAIT, &pin) == -A20_ERR_BAD_HANDLE);
    close_handle(delivered);
    destroy(replacement);
    __atomic_store_n(&callback_task.destroy_started, 1, __ATOMIC_RELEASE);
    a20_task_object_detach(&callback_task);
    close_handle(callback_handle);
    a20_task_object_task_release(&callback_task);
    assert(allocations == 0 && destroyed_keys >= 6);
    /* Self handle holds only the small identity, never its task. */
    task = new_task();
    h = a20_handle_install_task(&table, task, A20_OBJ_TASK, A20_RIGHT_WAIT);
    destroy(task);
    assert(allocations == 1);
    close_handle(h);
    assert(allocations == 0);
    /* Lazy publication and weak acquisition racing destruction/close. */
    task = new_task();
    pthread_t threads[4];
    for (unsigned i = 0; i < 4; ++i) {
        assert(proc_get(task));
        assert(!pthread_create(&threads[i], NULL, race, task));
    }
    for (unsigned i = 0; i < 2000; ++i)
        sched_yield();
    destroy(task);
    for (unsigned i = 0; i < 4; ++i)
        assert(!pthread_join(threads[i], NULL));
    assert(allocations == 0);
    puts("PASS Native task identity: PID reuse, debug alias, transfer "
         "ownership, failures, self-cycle, races");
}

__attribute__((noreturn)) void panic(const char *fmt, ...)
{ (void)fmt; abort(); }

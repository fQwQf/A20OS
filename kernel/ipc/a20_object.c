/*
 * A20OS Native ABI — Type-aware object lifetime for handle entries.
 * Design reference: docs/native-abi/03-handle.md §1.1
 *
 * This file lives in kernel/ipc/ (always compiled, unlike abi/native/) so
 * the channel endpoint implementation can release message-carried object
 * references even in kernel configurations without the Native syscall ABI.
 * Timer slots exist only in ABI=both builds; the weak no-ops below keep
 * linux-only links working.
 */
#include "core/types.h"
#include "core/refcount.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "ipc/ipc.h"
#include "ipc/handle_table.h"
#include "mm/vmo.h"
#include "ipc/objstats.h"
#include "ext/kep.h"

a20_objstats_t g_a20_objstats;

__attribute__((weak)) void a20_timer_object_ref(int slot) { (void)slot; }
__attribute__((weak)) void a20_timer_object_release(int slot) { (void)slot; }

int a20_object_is_vfile_backed(uint16_t type)
{
    return type == A20_OBJ_FILE || type == A20_OBJ_DIRECTORY ||
           type == A20_OBJ_PIPE_ENDPOINT || type == A20_OBJ_DEVICE ||
           type == A20_OBJ_SOCKET;
}

void a20_object_ref(void *object, uint16_t type)
{
    if (!object) return;
    if (a20_object_is_vfile_backed(type)) {
        vfs_ref_fd((int)(uintptr_t)object);
        return;
    }
    switch (type) {
    case A20_OBJ_CHANNEL_ENDPOINT:
        refcount_inc(&((a20_channel_ep_t *)object)->refcount);
        break;
    case A20_OBJ_EVENT_QUEUE:
        refcount_inc(&((a20_eventq_t *)object)->refcount);
        break;
    case A20_OBJ_MEMORY:
        vmo_ref((struct vmo *)object);
        break;
    case A20_OBJ_TIMER:
        a20_timer_object_ref((int)(uintptr_t)object - 1);
        break;
    case A20_OBJ_NAMESPACE:
        refcount_inc(&((struct a20_namespace *)object)->refcount);
        break;
    default:
        /* TASK/THREAD/DEBUG store integer pids — no backing object. */
        break;
    }
}

void a20_eventq_on_vfile_destroy(int fd)
{
    void *key = (void *)(uintptr_t)fd;
    a20_eventq_on_object_destroy(key, A20_OBJ_FILE);
    a20_eventq_on_object_destroy(key, A20_OBJ_DIRECTORY);
    a20_eventq_on_object_destroy(key, A20_OBJ_PIPE_ENDPOINT);
    a20_eventq_on_object_destroy(key, A20_OBJ_DEVICE);
    a20_eventq_on_object_destroy(key, A20_OBJ_SOCKET);
}

void a20_object_release(void *object, uint16_t type)
{
    if (!object) return;
    if (a20_object_is_vfile_backed(type)) {
        vfs_close((int)(uintptr_t)object);
        return;
    }
    switch (type) {
    case A20_OBJ_CHANNEL_ENDPOINT:
        a20_channel_ep_release((a20_channel_ep_t *)object);
        break;
    case A20_OBJ_EVENT_QUEUE:
        a20_eventq_release((a20_eventq_t *)object);
        break;
    case A20_OBJ_MEMORY:
        /* Native watchers key on the VMO object; fire before the final ref
         * drops so a destroyed object still notifies its event queue. */
        a20_eventq_on_object_destroy(object, A20_OBJ_MEMORY);
        vmo_release((struct vmo *)object);
        break;
    case A20_OBJ_TIMER:
        a20_timer_object_release((int)(uintptr_t)object - 1);
        break;
    case A20_OBJ_NAMESPACE:
        if (refcount_dec_and_test(&((struct a20_namespace *)object)->refcount)) {
            a20_eventq_on_object_destroy(object, A20_OBJ_NAMESPACE);
            kfree(object);
        }
        break;
    case A20_OBJ_EXT_PROG:
        /* Dropping the last handle releases the extension program; the
         * kernel drops its references on detach / owner exit. */
        (void)kep_prog_release((int)(intptr_t)object);
        break;
    default:
        break;
    }
}

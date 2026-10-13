#if !defined(CONFIG_MCU) || defined(CONFIG_MCU_NATIVE_PROGRAM)
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
#include "mm/vmar.h"
#include "core/types.h"
#include "core/refcount.h"
#include "mm/slab.h"
#include "fs/vfs.h"
#include "ipc/ipc.h"
#include "ipc/handle_table.h"
#include "mm/vmo.h"
#include "ipc/objstats.h"
#include "ext/kep.h"
#ifndef CONFIG_NATIVE_TASK_HANDLES_ONLY
#include "hyp/hyp_vcpu.h"
#endif
#include "proc/task_object.h"

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
#ifdef A20_TASK_OBJECTS_ENABLED
    if (type == A20_OBJ_TASK || type == A20_OBJ_THREAD ||
        type == A20_OBJ_DEBUG) {
        a20_task_object_ref(object);
        return;
    }
#endif
#ifdef CONFIG_NATIVE_TASK_HANDLES_ONLY
#ifdef CONFIG_NATIVE_FLAT_FILES
    if (type == A20_OBJ_FILE) {
        vfile_get((vfile_t *)object);
        return;
    }
#endif
    /* Other object types are absent from this minimal build. */
    (void)object;
    (void)type;
#else
    if (!object) return;
    if (a20_object_is_vfile_backed(type)) {
        /* Vfile-backed native objects are vfile pointers, each handle owns
         * one vfile reference (no fd slot is involved). */
        vfile_get((struct vfile *)object);
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
    case A20_OBJ_PAGER:
        a20_pager_ref((a20_pager_t *)object);
        break;
    case A20_OBJ_MONITOR:
        a20_monitor_ref((a20_monitor_t *)object);
        break;
    case A20_OBJ_HYP_VM:
        hyp_vm_get(object);
        break;
    case A20_OBJ_HYP_VCPU:
        hyp_vcpu_get(object);
        break;
    case A20_OBJ_VMAR:
        vmar_acquire((vmar_t *)object);
        break;
    default:
        /* No owned backing object for this type. */
        break;
    }
#endif
}

void a20_eventq_on_vfile_destroy(struct vfile *vf)
{
    a20_eventq_on_object_destroy(vf, A20_OBJ_FILE);
    a20_eventq_on_object_destroy(vf, A20_OBJ_DIRECTORY);
    a20_eventq_on_object_destroy(vf, A20_OBJ_PIPE_ENDPOINT);
    a20_eventq_on_object_destroy(vf, A20_OBJ_DEVICE);
    a20_eventq_on_object_destroy(vf, A20_OBJ_SOCKET);
}

void a20_object_release(void *object, uint16_t type)
{
#ifdef A20_TASK_OBJECTS_ENABLED
    if (type == A20_OBJ_TASK || type == A20_OBJ_THREAD ||
        type == A20_OBJ_DEBUG) {
        a20_task_object_put(object);
        return;
    }
#endif
#ifdef CONFIG_NATIVE_TASK_HANDLES_ONLY
#ifdef CONFIG_NATIVE_FLAT_FILES
    if (type == A20_OBJ_FILE) {
        vfs_put_file((vfile_t *)object);
        return;
    }
#endif
    /* Other object types are absent from this minimal build. */
    (void)object;
    (void)type;
#else
    if (!object) return;
    if (a20_object_is_vfile_backed(type)) {
        vfs_put_file((struct vfile *)object);
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
    case A20_OBJ_PAGER:
        a20_pager_put((a20_pager_t *)object);
        break;
    case A20_OBJ_MONITOR:
        a20_monitor_release((a20_monitor_t *)object);
        break;
    case A20_OBJ_HYP_VM:
        hyp_vm_put(object);
        break;
    case A20_OBJ_HYP_VCPU:
        hyp_vcpu_put(object);
        break;
    case A20_OBJ_VMAR:
        vmar_release((vmar_t *)object);
        break;
    case A20_OBJ_EXT_PROG:
        /* Dropping the last handle releases the extension program; the
         * kernel drops its references on detach / owner exit. */
        (void)kep_prog_release((int)(intptr_t)object);
        break;
    default:
        break;
    }
#endif
}

#endif

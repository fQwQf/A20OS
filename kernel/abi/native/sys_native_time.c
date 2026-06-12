/*
 * A20OS Native ABI — Phase 2 syscall implementations.
 *
 * This file is part of the mechanically split Native Phase 2 ABI.
 * See sys_phase2.c for shared helpers and forward declarations.
 */
#include "core/types.h"
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
#include "abi/native/objects.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"

#define A20_ARG(n) (args->arg[(n)])

extern struct a20_ht_internal *task_get_a20_ht(task_t *t);
extern int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                                  uint16_t type, a20_rights_t rights);
extern int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                           uint16_t type, a20_rights_t rights,
                                           uint64_t expiry_tick, uint32_t remaining_ops,
                                           uint32_t temporal_flags, uint8_t security_label);
extern int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                          uint16_t expected_type, a20_rights_t required_rights,
                                          a20_handle_entry_t *out);
extern void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);

/* ===== Time (0x0700) continued ===== */

/*
 * A20 timer object: wraps a kernel alarm into a handle-table entry.
 * On expiry the timer enqueues a pending_event into the user-supplied
 * event_queue (timer.md §3).  The implementation mirrors the Linux
 * posix_timer pattern but integrates with A20 handle/event semantics.
 */
#define A20_TIMER_MAX 64
typedef struct {
    volatile int used;
    int          owner_pid;
    uint64_t     interval_ticks;
    uint64_t     expire_tick;
    a20_handle_t event_queue;   /* handle of the target eventq */
    uint64_t     user_data;
    int          active;
} a20_timer_obj_t;

static a20_timer_obj_t g_a20_timers[A20_TIMER_MAX];

int64_t sys_a20_timer_create(const a20_syscall_args_t *args)
{
    a20_timer_create_args_t *uargs = (a20_timer_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_timer_create_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    /* Allocate a timer slot */
    int slot = -1;
    for (int i = 0; i < A20_TIMER_MAX; i++) {
        if (!g_a20_timers[i].used) { slot = i; break; }
    }
    if (slot < 0) return -A20_ERR_NO_MEMORY;

    task_t *cur = proc_current();
    a20_timer_obj_t *t = &g_a20_timers[slot];
    memset(t, 0, sizeof(*t));
    t->used = 1;
    t->owner_pid = cur ? cur->pid : 0;
    t->event_queue = kargs.event_queue;
    t->user_data = kargs.user_data;

    /* Install as a handle so the user can refer to it */
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) { t->used = 0; return -A20_ERR_BAD_HANDLE; }

    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)slot, A20_OBJ_TIMER,
                                    A20_RIGHT_READ | A20_RIGHT_CONTROL |
                                    A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
    if (h < 0) { t->used = 0; return h; }

    kargs.out_timer = (a20_handle_t)h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_timer_set(const a20_syscall_args_t *args)
{
    a20_handle_t timer_h = (a20_handle_t)A20_ARG(0);
    uint64_t deadline_ns = A20_ARG(1);
    uint64_t interval_ns = A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, timer_h, A20_OBJ_TIMER,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    int slot = (int)(uintptr_t)entry.object;
    if (slot < 0 || slot >= A20_TIMER_MAX || !g_a20_timers[slot].used)
        return -A20_ERR_BAD_HANDLE;

    a20_timer_obj_t *t = &g_a20_timers[slot];
    if (deadline_ns == 0) {
        /* Disarm */
        t->active = 0;
        t->expire_tick = 0;
        return A20_OK;
    }

    uint64_t now_ns = timer_get_ticks() * (1000000000ULL / TICKS_PER_SEC);
    uint64_t delta_ns = (deadline_ns > now_ns) ? (deadline_ns - now_ns) : 1;
    t->expire_tick = timer_get_ticks() +
                     delta_ns * TICKS_PER_SEC / 1000000000ULL;
    t->interval_ticks = interval_ns * TICKS_PER_SEC / 1000000000ULL;
    t->active = 1;

    /* If an event_queue is associated, set a kernel alarm so we get
     * woken.  For simplicity we piggyback on the task alarm mechanism. */
    if (cur && t->expire_tick > 0) {
        proc_set_alarm_expire(cur, t->expire_tick);
    }

    return A20_OK;
}

int64_t sys_a20_timer_cancel(const a20_syscall_args_t *args)
{
    a20_handle_t timer_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_internal(ht, timer_h, A20_OBJ_TIMER,
                                            A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    int slot = (int)(uintptr_t)entry.object;
    if (slot < 0 || slot >= A20_TIMER_MAX || !g_a20_timers[slot].used)
        return -A20_ERR_BAD_HANDLE;

    g_a20_timers[slot].active = 0;
    g_a20_timers[slot].expire_tick = 0;
    g_a20_timers[slot].interval_ticks = 0;
    return A20_OK;
}

/* Called from scheduler tick to fire expired A20 timers.
 * Notifies the associated event queue for each expired timer. */
void a20_timer_tick(void)
{
    uint64_t now = timer_get_ticks();
    for (int i = 0; i < A20_TIMER_MAX; i++) {
        a20_timer_obj_t *t = &g_a20_timers[i];
        if (!t->used || !t->active || t->expire_tick == 0)
            continue;
        if (now < t->expire_tick)
            continue;

        a20_event_notify(t, A20_OBJ_TIMER, 0, t->user_data, t->expire_tick);

        if (t->interval_ticks > 0) {
            t->expire_tick = now + t->interval_ticks;
        } else {
            t->active = 0;
            t->expire_tick = 0;
        }
    }
}

int64_t sys_a20_clock_set(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)A20_ARG(0);
    a20_time_ns_t value = (a20_time_ns_t)A20_ARG(1);
    (void)value;
    if (clock_id > 1) return -A20_ERR_INVALID_ARGUMENT;
    return -A20_ERR_PERM;
}

int64_t sys_a20_clock_resolution(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)A20_ARG(0);
    a20_time_ns_t *out = (a20_time_ns_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    a20_time_ns_t res = 1000000;
    if (clock_id == 0 || clock_id == 1) res = 1;
    if (copy_to_user(out, &res, sizeof(res)) < 0) return -A20_ERR_FAULT;
    return A20_OK;
}


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
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "sys_validate.h"
#include "abi/native/startup.h"
#include "abi/native/vmo.h"
#include "abi/native/vmar.h"
#include "abi/native/ipc_internal.h"
#include "abi/native/resource.h"
#include "handle_table.h"

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
extern int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
extern uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
extern void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

extern int copy_path_from_user(char *dst, const char *uptr, uint32_t len);
extern void resolve_path(const char *in, char *out);
extern int64_t sys_a20_path_open(const a20_syscall_args_t *args);
extern void a20_temporal_sweep_all(void);

/* ===== Time (0x0700) continued ===== */

/*
 * A20 timer object: wraps a kernel alarm into a handle-table entry.
 * On expiry the timer enqueues a pending_event into the user-supplied
 * event_queue (timer.md §3).  The implementation mirrors the Linux
 * posix_timer pattern but integrates with A20 handle/event semantics.
 *
 * Handle entries store (void*)(uintptr_t)(slot + 1) so that slot 0 is a
 * valid non-NULL object.  The slot array is refcounted: dup/transfer take
 * references, and the final release frees the slot.
 */
#define A20_TIMER_MAX 64
typedef struct {
    volatile int used;
    refcount_t   refcount;
    int          owner_pid;
    uint64_t     interval_ticks;
    uint64_t     expire_tick;
    a20_handle_t event_queue;   /* handle of the target eventq */
    uint64_t     user_data;
    int          active;
    int          closing;
} a20_timer_obj_t;

static a20_timer_obj_t g_a20_timers[A20_TIMER_MAX];
static spinlock_t g_a20_timers_lock = SPINLOCK_INIT;

void a20_timer_object_ref(int slot)
{
    if (slot < 0 || slot >= A20_TIMER_MAX) return;
    uint64_t flags = spin_lock_irqsave(&g_a20_timers_lock);
    if (g_a20_timers[slot].used && !g_a20_timers[slot].closing)
        refcount_inc(&g_a20_timers[slot].refcount);
    spin_unlock_irqrestore(&g_a20_timers_lock, flags);
}

void a20_timer_object_release(int slot)
{
    if (slot < 0 || slot >= A20_TIMER_MAX) return;
    uint64_t flags = spin_lock_irqsave(&g_a20_timers_lock);
    a20_timer_obj_t *t = &g_a20_timers[slot];
    if (t->used && !t->closing && refcount_dec_and_test(&t->refcount)) {
        t->active = 0;
        t->closing = 1;
        spin_unlock_irqrestore(&g_a20_timers_lock, flags);
        /* Watch keys use the handle entry object: (void *)(slot + 1).  Keep
         * the slot closed across teardown so its numeric key cannot be
         * reused while stale watch entries are still being removed. */
        a20_eventq_on_object_destroy((void *)(uintptr_t)(slot + 1), A20_OBJ_TIMER);
        flags = spin_lock_irqsave(&g_a20_timers_lock);
        memset(t, 0, sizeof(*t));
        spin_unlock_irqrestore(&g_a20_timers_lock, flags);
        return;
    }
    spin_unlock_irqrestore(&g_a20_timers_lock, flags);
}

int64_t sys_a20_timer_create(const a20_syscall_args_t *args)
{
    a20_timer_create_args_t *uargs = (a20_timer_create_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_timer_create_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* Allocate a timer slot */
    uint64_t tflags = spin_lock_irqsave(&g_a20_timers_lock);
    int slot = -1;
    for (int i = 0; i < A20_TIMER_MAX; i++) {
        if (!g_a20_timers[i].used && !g_a20_timers[i].closing) { slot = i; break; }
    }
    if (slot >= 0) {
        a20_timer_obj_t *t = &g_a20_timers[slot];
        memset(t, 0, sizeof(*t));
        t->used = 1;
        refcount_set(&t->refcount, 1);
        t->owner_pid = cur ? cur->pid : 0;
        t->event_queue = kargs.event_queue;
        t->user_data = kargs.user_data;
    }
    spin_unlock_irqrestore(&g_a20_timers_lock, tflags);
    if (slot < 0) return -A20_ERR_NO_MEMORY;

    /* Install as a handle so the user can refer to it (slot + 1: the entry
     * object must be non-NULL, see a20_timer_obj_t comment). */
    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)(slot + 1), A20_OBJ_TIMER,
                                    A20_RIGHT_READ | A20_RIGHT_CONTROL |
                                    A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
    if (h < 0) { a20_timer_object_release(slot); return h; }

    kargs.out_timer = (a20_handle_t)h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)h);
        return -A20_ERR_FAULT;
    }
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
    int64_t r = a20_handle_lookup_ref_internal(ht, timer_h, A20_OBJ_TIMER,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    int slot = (int)(uintptr_t)entry.object - 1;
    uint64_t expire_tick = 0;
    uint64_t interval_ticks = interval_ns * TICKS_PER_SEC / 1000000000ULL;
    if (deadline_ns != 0) {
        uint64_t now_ns = timer_get_ticks() * (1000000000ULL / TICKS_PER_SEC);
        uint64_t delta_ns = (deadline_ns > now_ns) ? (deadline_ns - now_ns) : 1;
        expire_tick = timer_get_ticks() +
                      delta_ns * TICKS_PER_SEC / 1000000000ULL;
    }

    uint64_t flags = spin_lock_irqsave(&g_a20_timers_lock);
    if (slot < 0 || slot >= A20_TIMER_MAX ||
        !g_a20_timers[slot].used || g_a20_timers[slot].closing) {
        spin_unlock_irqrestore(&g_a20_timers_lock, flags);
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_BAD_HANDLE;
    }

    a20_timer_obj_t *t = &g_a20_timers[slot];
    if (deadline_ns == 0) {
        t->active = 0;
        t->expire_tick = 0;
        t->interval_ticks = 0;
    } else {
        t->expire_tick = expire_tick;
        t->interval_ticks = interval_ticks;
        t->active = 1;
    }
    spin_unlock_irqrestore(&g_a20_timers_lock, flags);

    /* Ask the scheduler to run a20_timer_tick() at the deadline.  Unlike
     * proc_set_alarm_expire this does not arm a SIGALRM on the caller —
     * A20 timers deliver events, not signals. */
    if (expire_tick > 0)
        sched_note_timer_deadline(expire_tick);

    a20_object_release(entry.object, entry.type);
    return A20_OK;
}


int64_t sys_a20_timer_cancel(const a20_syscall_args_t *args)
{
    a20_handle_t timer_h = (a20_handle_t)A20_ARG(0);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, timer_h, A20_OBJ_TIMER,
                                               A20_RIGHT_CONTROL, &entry);
    if (r < 0) return r;

    int slot = (int)(uintptr_t)entry.object - 1;
    uint64_t flags = spin_lock_irqsave(&g_a20_timers_lock);
    if (slot < 0 || slot >= A20_TIMER_MAX ||
        !g_a20_timers[slot].used || g_a20_timers[slot].closing) {
        spin_unlock_irqrestore(&g_a20_timers_lock, flags);
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_BAD_HANDLE;
    }

    g_a20_timers[slot].active = 0;
    g_a20_timers[slot].expire_tick = 0;
    g_a20_timers[slot].interval_ticks = 0;
    spin_unlock_irqrestore(&g_a20_timers_lock, flags);
    a20_object_release(entry.object, entry.type);
    return A20_OK;
}


/*
 * a20_timer_tick — called from the scheduler tick path (sched(), process
 * context).  Fires expired A20 timers and periodically runs the temporal
 * sweeper across all handle tables (docs/native-abi/03-handle.md §2.6.4).
 * Each sweep re-arms the next scan deadline so the cadence is sustained
 * even with no armed timers or task alarms.
 */
void a20_timer_tick(void)
{
    uint64_t now = timer_get_ticks();

    for (int i = 0; i < A20_TIMER_MAX; i++) {
        uint64_t user_data = 0;
        uint64_t fired_tick = 0;
        uint64_t next_tick = 0;
        int fire = 0;

        uint64_t flags = spin_lock_irqsave(&g_a20_timers_lock);
        a20_timer_obj_t *t = &g_a20_timers[i];
        if (t->used && !t->closing && t->active &&
            t->expire_tick != 0 && now >= t->expire_tick) {
            fire = 1;
            refcount_inc(&t->refcount);
            user_data = t->user_data;
            fired_tick = t->expire_tick;
            if (t->interval_ticks > 0) {
                next_tick = now + t->interval_ticks;
                t->expire_tick = next_tick;
            } else {
                t->active = 0;
                t->expire_tick = 0;
            }
        }
        spin_unlock_irqrestore(&g_a20_timers_lock, flags);

        if (!fire)
            continue;

        /* The watch key is the handle entry's object: (void *)(slot + 1). */
        a20_event_notify((void *)(uintptr_t)(i + 1), A20_OBJ_TIMER,
                         A20_EVENT_EXPIRED, user_data, fired_tick);
        if (next_tick > 0)
            sched_note_timer_deadline(next_tick);
        a20_timer_object_release(i);
    }

    static uint64_t last_sweep;
    if (now - last_sweep >= A20_SWEEP_INTERVAL_TICKS) {
        last_sweep = now;
        a20_temporal_sweep_all();
        sched_note_timer_deadline(now + A20_SWEEP_INTERVAL_TICKS);
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

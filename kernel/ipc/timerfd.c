#include "ipc/timerfd.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"

#include "core/poll.h"
#include "core/consts.h"
#include "core/lock.h"
#include "core/sync.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/readiness.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"

typedef struct {
    spinlock_t    lock;
    wait_queue_t  waiters;
    uint64_t interval_sec, interval_nsec;
    uint64_t expire_tick;
    int armed;
    int nonblock;
    int clockid;
    int cancel_on_set;
    uint64_t cancel_gen;
} timerfd_t;

/* State points at timerfd_t.interval_sec; the trailing fields through
 * cancel_gen are the Rust repr(C) borrowed-view contract. */
#define TIMERFD_STATE_OFFSET(member) \
    ((size_t)__builtin_offsetof(timerfd_t, member) - \
     (size_t)__builtin_offsetof(timerfd_t, interval_sec))
STATIC_ASSERT(TIMERFD_STATE_OFFSET(expire_tick) == 16, timerfd_rust_expire);
STATIC_ASSERT(TIMERFD_STATE_OFFSET(armed) == 24, timerfd_rust_armed);
STATIC_ASSERT(TIMERFD_STATE_OFFSET(nonblock) == 28, timerfd_rust_nonblock);
STATIC_ASSERT(TIMERFD_STATE_OFFSET(clockid) == 32, timerfd_rust_clockid);
STATIC_ASSERT(TIMERFD_STATE_OFFSET(cancel_on_set) == 36, timerfd_rust_cancel);
STATIC_ASSERT(TIMERFD_STATE_OFFSET(cancel_gen) == 40, timerfd_rust_cancel_gen);
STATIC_ASSERT(sizeof(timerfd_t) - (size_t)__builtin_offsetof(timerfd_t, interval_sec) == 48,
              timerfd_rust_state_size);
#undef TIMERFD_STATE_OFFSET
int a20_timerfd_read_txn(void *state, void *context, uint64_t ticks_per_sec,
                         uint64_t *value_out);
int a20_timerfd_poll_txn(void *state, void *context, int events);
uint64_t a20_timerfd_poll_deadline_txn(void *state, void *context);
int a20_timerfd_gettime_txn(void *state, void *context,
                            uint64_t ticks_per_sec, uint64_t *curr_value);
int a20_timerfd_settime_txn(void *state, void *context, int flags,
                            const uint64_t *new_value, uint64_t *old_value,
                            uint64_t ticks_per_sec);
void a20_timerfd_state_init_rust(void *state, int clockid, bool nonblock);

typedef struct {
    proc_wait_token_t token;
    wait_queue_entry_t entry;
    wait_queue_t *queue;
} timerfd_waiter_t;

void a20_timerfd_txn_lock(void *context)
{
    timerfd_t *tfd = context;
    spin_lock(&tfd->lock);
}

void a20_timerfd_txn_unlock(void *context)
{
    timerfd_t *tfd = context;
    spin_unlock(&tfd->lock);
}

void *a20_timerfd_txn_wait_prepare(void *context, uint64_t deadline)
{
    (void)context;
    timerfd_waiter_t *waiter = kmalloc(sizeof(*waiter));
    if (!waiter)
        return NULL;
    memset(waiter, 0, sizeof(*waiter));
    waiter->token = proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, deadline);
    if (!waiter->token.task) {
        kfree(waiter);
        return NULL;
    }
    return waiter;
}

bool a20_timerfd_txn_wait_link(void *context, void *opaque)
{
    timerfd_t *tfd = context;
    timerfd_waiter_t *waiter = opaque;
    bool linked = wait_queue_link(&tfd->waiters, &waiter->entry,
                                  waiter->token, 0);
    waiter->queue = linked ? &tfd->waiters : NULL;
    return linked;
}

int a20_timerfd_txn_wait_finish(void *opaque, bool linked, bool commit)
{
    timerfd_waiter_t *waiter = opaque;
    proc_wake_reason_t reason = PROC_WAKE_CANCEL;
    if (commit && linked)
        reason = proc_park_commit(waiter->token);
    else
        (void)proc_park_cancel(waiter->token);
    if (linked && waiter->queue)
        wait_queue_unlink(waiter->queue, &waiter->entry);
    proc_park_finish(waiter->token);
    int interrupted = proc_wake_reason_is_task_interrupt(reason);
    kfree(waiter);
    return interrupted ? -ERESTARTSYS : 0;
}

void a20_timerfd_txn_wake(void *context)
{
    timerfd_t *tfd = context;
    wait_queue_wake_all(&tfd->waiters, 0, PROC_WAKE_EVENT);
}

uint64_t a20_timerfd_txn_now_ticks(void *context)
{
    (void)context;
    return timer_get_ticks();
}

void a20_timerfd_txn_now_timespec(void *context, int clockid, uint64_t out[2])
{
    (void)context;
    if (clockid == 0)
        timekeeping_get_realtime(out);
    else
        timekeeping_get_monotonic(out);
}

uint64_t a20_timerfd_txn_realtime_generation(void *context)
{
    (void)context;
    return timekeeping_realtime_set_generation();
}

static int timerfd_read(vfile_t *vf, char *buf, size_t count)
{
    timerfd_t *tfd = vf ? vf->priv : NULL;
    if (!tfd) return -EBADF;
    if (count < sizeof(uint64_t)) return -EINVAL;

    uint64_t expirations;
    int result = a20_timerfd_read_txn(&tfd->interval_sec, tfd, TICKS_PER_SEC,
                                      &expirations);
    if (result < 0)
        return result;
    memcpy(buf, &expirations, sizeof(expirations));
    return sizeof(expirations);
}

static int timerfd_poll(vfile_t *vf, short events)
{
    timerfd_t *tfd = vf ? vf->priv : NULL;
    if (!tfd) return POLLNVAL;

    return a20_timerfd_poll_txn(&tfd->interval_sec, tfd, events);
}

static size_t timerfd_poll_sources(vfile_t *vf, short events,
                                   readiness_source_t *sources, size_t max)
{
    timerfd_t *tfd = vf ? vf->priv : NULL;
    if (!tfd || !sources || max == 0 || !(events & POLLIN))
        return 0;
    uint64_t deadline;
    deadline = a20_timerfd_poll_deadline_txn(&tfd->interval_sec, tfd);
    sources[0] = (readiness_source_t){
        .queue = &tfd->waiters,
        .key = 0,
        .deadline = deadline,
    };
    return 1;
}

static vfile_ops_t g_timerfd_ops = {
    .read = timerfd_read,
    .poll = timerfd_poll,
    .poll_sources = timerfd_poll_sources,
    .close = anonfd_free_priv_close,
};

int timerfd_create_file(int clockid, int flags)
{
    /* CLOCK_REALTIME/MONOTONIC/BOOTTIME; the *_ALARM clocks need a
     * suspend/resume wake source this kernel does not have. */
    if (clockid != 0 && clockid != 1 && clockid != 7) return -EINVAL;
    if (flags & ~(O_CLOEXEC | O_NONBLOCK)) return -EINVAL;
    timerfd_t *tfd = kmalloc(sizeof(*tfd));
    vfile_t *vf = vfile_alloc();
    if (!tfd || !vf) {
        if (tfd) kfree(tfd);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    memset(tfd, 0, sizeof(*tfd));
    spin_init(&tfd->lock);
    wait_queue_init(&tfd->waiters);
    a20_timerfd_state_init_rust(&tfd->interval_sec, clockid,
                                (flags & O_NONBLOCK) != 0);
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_timerfd_ops;
    vf->priv = tfd;
    if (env_kind_tag_vfile(vf, A20_OBJ_TIMER) < 0) {
        vfile_free(vf);
        kfree(tfd);
        return -EINVAL;
    }
    return anonfd_install_vfile(vf, flags);
}

int timerfd_settime_file(int gfd, int flags, const uint64_t new_value[4], uint64_t old_value[4])
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf) return -EINVAL;
    if (vf->ops != &g_timerfd_ops) {
        vfs_put_file_ref(gfd, vf);
        return -EINVAL;
    }
    timerfd_t *tfd = vf->priv;
    int result = a20_timerfd_settime_txn(&tfd->interval_sec, tfd, flags,
                                         new_value, old_value, TICKS_PER_SEC);
    vfs_put_file_ref(gfd, vf);
    return result;
}

int timerfd_gettime_file(int gfd, uint64_t curr_value[4])
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf) return -EINVAL;
    if (vf->ops != &g_timerfd_ops) {
        vfs_put_file_ref(gfd, vf);
        return -EINVAL;
    }
    timerfd_t *tfd = vf->priv;
    int result = a20_timerfd_gettime_txn(&tfd->interval_sec, tfd,
                                         TICKS_PER_SEC, curr_value);
    vfs_put_file_ref(gfd, vf);
    return result;
}

#include "ipc/signalfd.h"
#include "ipc/envelope.h"
#include "ipc/ipc.h"

#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/readiness.h"
#include "fs/fdtable.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "proc/signal_core.h"
#include "core/errno.h"
#include "core/poll.h"

/*
 * struct signalfd_siginfo, Linux layout (128 bytes).  Userspace normally
 * only consumes ssi_signo; ssi_errno/ssi_code/ssi_pid/ssi_uid are filled
 * best-effort from the queued arch siginfo.
 */
typedef struct {
    uint32_t ssi_signo;
    int32_t  ssi_errno;
    int32_t  ssi_code;
    uint32_t ssi_pid;
    uint32_t ssi_uid;
    int32_t  ssi_fd;
    uint32_t ssi_tid;
    uint32_t ssi_band;
    uint32_t ssi_overrun;
    uint32_t ssi_trapno;
    int32_t  ssi_status;
    int32_t  ssi_int;
    uint64_t ssi_ptr;
    uint64_t ssi_utime;
    uint64_t ssi_stime;
    uint64_t ssi_addr;
    uint16_t ssi_addr_lsb;
    uint16_t __pad2;
    int32_t  ssi_syscall;
    uint64_t ssi_call_addr;
    uint32_t ssi_arch;
    uint8_t  __pad[28];
} signalfd_siginfo_t;

_Static_assert(sizeof(signalfd_siginfo_t) == 128,
               "signalfd_siginfo must stay 128 bytes");

typedef struct {
    spinlock_t lock;
    uint64_t mask; /* kernel encoding: bit position == signum */
} signalfd_t;

void a20_signalfd_set_mask_rust(uint64_t *mask, uint64_t value);
uint64_t a20_signalfd_mask_rust(const uint64_t *mask);
bool a20_signalfd_has_pending_rust(uint64_t pending, uint64_t mask);

static int signalfd_is_ops(const vfile_t *vf);

/* Whether a read of `count` bytes can return at least one record. */
static int signalfd_read_valid(size_t count, size_t record_size)
{
    return a20_signal_fd_read_valid(count, record_size);
}


/* The vfile reference protects lifetime only; this lock serializes mask
 * updates with read/poll snapshots across duplicated and inherited fds. */
static uint64_t signalfd_mask_snapshot(signalfd_t *sfd)
{
    if (!sfd)
        return 0;
    spin_lock(&sfd->lock);
    uint64_t mask = a20_signalfd_mask_rust(&sfd->mask);
    spin_unlock(&sfd->lock);
    return mask;
}


static int signalfd_read(vfile_t *vf, char *buf, size_t count)
{
    signalfd_t *sfd = vf ? vf->priv : NULL;
    if (!sfd) return -EBADF;
    if (!signalfd_read_valid(count, sizeof(signalfd_siginfo_t))) return -EINVAL;
    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    size_t done = 0;
    uint64_t mask = 0;
    for (;;) {
        /* Rust owns the whole signalfd select/consume/record transaction:
         * dequeue up to one batch per locked call, then copy out. */
        mask = signalfd_mask_snapshot(sfd);
        signalfd_siginfo_t batch[8];
        size_t capacity = (count - done) / sizeof(signalfd_siginfo_t);
        if (capacity > sizeof(batch) / sizeof(batch[0]))
            capacity = sizeof(batch) / sizeof(batch[0]);
        uint64_t batch_flags = spin_lock_irqsave(&ss->lock);
        a20_signal_queue_state_t st = signal_queue_state_fill(t, ss);
        int64_t got = a20_signal_fd_read_locked(&st, mask, (uint8_t *)batch,
                                                capacity,
                                                sizeof(signalfd_siginfo_t),
                                                offsetof(arch_siginfo_t, _sifields));
        spin_unlock_irqrestore(&ss->lock, batch_flags);
        if (got > 0) {
            memcpy(buf + done, batch, (size_t)got * sizeof(signalfd_siginfo_t));
            done += (size_t)got * sizeof(signalfd_siginfo_t);
            continue;
        }
        if (done)
            break;
        if (vf->flags & O_NONBLOCK)
            return -EAGAIN;

        /*
         * Blocking path: same park protocol as sys_sigtimedwait().  The
         * signal queueing path wakes tasks whose sigwait_mask matches the
         * incoming signal.
         */
        proc_wait_token_t token = {0};
        /* park_lock -> signal_state.lock is the documented order; no global
         * task lock is involved in the park protocol. */
        uint64_t park_flags = spin_lock_irqsave(&t->park_lock);
        mask = signalfd_mask_snapshot(sfd);
        uint64_t sflags = spin_lock_irqsave(&ss->lock);
        uint64_t pending = ss->pending | t->thread_pending;
        bool matching = a20_signalfd_has_pending_rust(pending, mask);
        if (!matching) {
            t->sigwait_mask = mask;
            t->sigwait_active = 1;
        }
        spin_unlock_irqrestore(&ss->lock, sflags);
        if (!matching)
            token = proc_park_prepare_locked(PROC_WAIT_INTERRUPTIBLE, 0);
        spin_unlock_irqrestore(&t->park_lock, park_flags);

        if (matching)
            continue;
        if (!token.task) {
            sflags = spin_lock_irqsave(&ss->lock);
            t->sigwait_active = 0;
            t->sigwait_mask = 0;
            spin_unlock_irqrestore(&ss->lock, sflags);
            return -EAGAIN;
        }

        /* Subscribe to mask/readiness changes before the final pending check.
         * This closes both the signal-arrives-before-park and mask-update
         * races without holding the signal-state lock while parking.
         */
        wait_queue_entry_t entry = {0};
        bool linked = wait_queue_link(&ss->readiness_waiters, &entry,
                                      token, 0);
        if (!linked) {
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            sflags = spin_lock_irqsave(&ss->lock);
            t->sigwait_active = 0;
            t->sigwait_mask = 0;
            spin_unlock_irqrestore(&ss->lock, sflags);
            return -EAGAIN;
        }

        mask = signalfd_mask_snapshot(sfd);
        sflags = spin_lock_irqsave(&ss->lock);
        bool ready_now = a20_signalfd_has_pending_rust(
            ss->pending | t->thread_pending, mask);
        if (ready_now) {
            t->sigwait_active = 0;
            t->sigwait_mask = 0;
        }
        spin_unlock_irqrestore(&ss->lock, sflags);
        proc_wake_reason_t reason;
        if (ready_now) {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        } else {
            reason = proc_park_commit(token);
        }
        wait_queue_unlink(&ss->readiness_waiters, &entry);
        proc_park_finish(token);

        mask = signalfd_mask_snapshot(sfd);
        sflags = spin_lock_irqsave(&ss->lock);
        t->sigwait_active = 0;
        t->sigwait_mask = 0;
        int has_matching;
        has_matching = a20_signalfd_has_pending_rust(
            ss->pending | t->thread_pending, mask);
        spin_unlock_irqrestore(&ss->lock, sflags);

        if (proc_wake_reason_is_task_interrupt(reason) &&
            !has_matching &&
            signal_task_has_unblocked(t))
            return -EINTR;
        /* A matching queued signal is consumed at the top of the loop. */
    }
    return (int)done;
}

static int signalfd_write(vfile_t *vf, const char *buf, size_t count)
{
    (void)vf; (void)buf; (void)count;
    return -EINVAL;
}

static size_t signalfd_poll_sources(vfile_t *vf, short events,
                                    readiness_source_t *sources, size_t max)
{
    if (!signalfd_is_ops(vf) || !sources || max == 0 || !(events & POLLIN))
        return 0;
    task_t *task = proc_current();
    if (!task || !task->signals)
        return 0;
    signal_state_t *ss = (signal_state_t *)task->signals;
    sources[0] = (readiness_source_t){ &ss->readiness_waiters, 0, 0 };
    return 1;
}

static vfile_ops_t g_signalfd_ops = {
    .read = signalfd_read,
    .write = signalfd_write,
    .poll_sources = signalfd_poll_sources,
    .close = anonfd_free_priv_close,
};

static int signalfd_is_ops(const vfile_t *vf)
{
    return vf && vf->ops == &g_signalfd_ops;
}

int signalfd_poll_events(vfile_t *vf, short events)
{
    if (!signalfd_is_ops(vf))
        return -1;
    signalfd_t *sfd = vf->priv;
    int revents = 0;
    if (events & POLLIN) {
        task_t *t = proc_current();
        if (t && t->signals) {
            signal_state_t *ss = (signal_state_t *)t->signals;
            uint64_t sflags = spin_lock_irqsave(&ss->lock);
            uint64_t mask = signalfd_mask_snapshot(sfd);
            if (a20_signalfd_has_pending_rust(
                    ss->pending | t->thread_pending, mask))
                revents |= POLLIN;
            spin_unlock_irqrestore(&ss->lock, sflags);
        }
    }
    return revents;
}

int signalfd_create(int ufd, uint64_t kernel_mask, int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
        return -EINVAL;

    if (ufd != -1) {
        /* Update the mask of an existing signalfd. */
        int64_t gfd = fdtable_get_current(ufd);
        if (gfd < 0) return -EBADF;
        vfile_t *vf = vfs_get_file_ref((int)gfd);
        if (!vf) return -EBADF;
        if (!signalfd_is_ops(vf)) {
            vfs_put_file_ref((int)gfd, vf);
            return -EINVAL;
        }
        signalfd_t *sfd = vf->priv;
        spin_lock(&sfd->lock);
        a20_signalfd_set_mask_rust(&sfd->mask, kernel_mask);
        spin_unlock(&sfd->lock);
        task_t *task = proc_current();
        if (task && task->signals) {
            signal_state_t *ss = task->signals;
            wait_queue_wake_all(&ss->readiness_waiters, 0,
                                PROC_WAKE_EVENT);
        }
        vfs_put_file_ref((int)gfd, vf);
        return ufd;
    }

    signalfd_t *sfd = kmalloc(sizeof(*sfd));
    vfile_t *vf = vfile_alloc();
    if (!sfd || !vf) {
        if (sfd) kfree(sfd);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    spin_init(&sfd->lock);
    sfd->mask = kernel_mask;
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_signalfd_ops;
    vf->priv = sfd;
    if (env_kind_tag_vfile(vf, A20_OBJ_EVENT_QUEUE) < 0) {
        vfile_free(vf);
        kfree(sfd);
        return -EINVAL;
    }
    return anonfd_install_vfile(vf, flags);
}

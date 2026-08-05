#include "ipc/signalfd.h"

#include "core/lock.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/fdtable.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
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
    uint64_t mask; /* kernel encoding: bit position == signum */
} signalfd_t;

static int signalfd_is_ops(const vfile_t *vf);

/*
 * Dequeue one pending signal matching mask from the calling task, without
 * running any handler.  Mirrors the selection logic in sys_sigtimedwait().
 * Returns the signal number, or 0 when nothing matches.
 */
static int signalfd_dequeue(task_t *t, uint64_t mask,
                            signalfd_siginfo_t *out)
{
    signal_state_t *ss = (signal_state_t *)t->signals;
    uint64_t sflags = spin_lock_irqsave(&ss->lock);
    uint64_t matching = (ss->pending | t->thread_pending) & mask;
    int selected = 0;
    if (matching) {
        for (int sig = 1; sig < NSIG; sig++) {
            if (!(matching & signal_mask_bit(sig)))
                continue;
            selected = sig;
            ss->pending &= ~signal_mask_bit(sig);
            t->thread_pending &= ~signal_mask_bit(sig);
            memset(out, 0, sizeof(*out));
            out->ssi_signo = (uint32_t)sig;
            if (ss->pending_has_info[sig]) {
                const uint8_t *pi = ss->pending_info[sig];
                int32_t v;
                memcpy(&v, pi + 4, 4);
                out->ssi_errno = v;
                memcpy(&v, pi + 8, 4);
                out->ssi_code = v;
                memcpy(&v, pi + 16, 4);
                out->ssi_pid = (uint32_t)v;
                memcpy(&v, pi + 20, 4);
                out->ssi_uid = (uint32_t)v;
            }
            ss->pending_has_info[sig] = 0;
            break;
        }
    }
    spin_unlock_irqrestore(&ss->lock, sflags);
    return selected;
}

static int signalfd_read(vfile_t *vf, char *buf, size_t count)
{
    signalfd_t *sfd = vf ? vf->priv : NULL;
    if (!sfd) return -EBADF;
    if (count < sizeof(signalfd_siginfo_t)) return -EINVAL;
    task_t *t = proc_current();
    if (!t || !t->signals) return -EINVAL;
    signal_state_t *ss = (signal_state_t *)t->signals;

    size_t done = 0;
    for (;;) {
        signalfd_siginfo_t fdsi;
        int sig = signalfd_dequeue(t, sfd->mask, &fdsi);
        if (sig) {
            memcpy(buf + done, &fdsi, sizeof(fdsi));
            done += sizeof(fdsi);
            if (done + sizeof(fdsi) > count)
                break;
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
        uint64_t proc_flags = spin_lock_irqsave(&proc_lock);
        uint64_t sflags = spin_lock_irqsave(&ss->lock);
        uint64_t matching = (ss->pending | t->thread_pending) & sfd->mask;
        if (!matching) {
            t->sigwait_mask = sfd->mask;
            t->sigwait_active = 1;
        }
        spin_unlock_irqrestore(&ss->lock, sflags);
        if (!matching)
            token = proc_park_prepare_locked(PROC_WAIT_INTERRUPTIBLE, 0);
        spin_unlock_irqrestore(&proc_lock, proc_flags);

        if (matching)
            continue;
        if (!token.task) {
            sflags = spin_lock_irqsave(&ss->lock);
            t->sigwait_active = 0;
            t->sigwait_mask = 0;
            spin_unlock_irqrestore(&ss->lock, sflags);
            return -EAGAIN;
        }

        proc_wake_reason_t reason = proc_park_commit(token);
        proc_park_finish(token);

        sflags = spin_lock_irqsave(&ss->lock);
        t->sigwait_active = 0;
        t->sigwait_mask = 0;
        int has_matching = ((ss->pending | t->thread_pending) & sfd->mask) != 0;
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

static vfile_ops_t g_signalfd_ops = {
    .read = signalfd_read,
    .write = signalfd_write,
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
            if ((ss->pending | t->thread_pending) & sfd->mask)
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
        sfd->mask = kernel_mask;
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
    sfd->mask = kernel_mask;
    vf->flags = O_RDONLY | (flags & O_NONBLOCK);
    refcount_set(&vf->ref_count, 1);
    vf->ops = &g_signalfd_ops;
    vf->priv = sfd;
    return anonfd_install_vfile(vf, flags);
}

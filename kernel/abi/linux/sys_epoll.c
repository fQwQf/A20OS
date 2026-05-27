#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/proc_internal.h"
#include "fs/vfs/file.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "proc/signal.h"

typedef struct {
    uint32_t events;
    uint64_t data;
} epoll_event_t;

#define EPOLL_CTL_ADD 1
#define EPOLL_CTL_DEL 2
#define EPOLL_CTL_MOD 3

#define EPOLL_MAX_FDS  1024
#define EPOLL_MAX_NESTING 4

#define EPOLLIN     0x001
#define EPOLLOUT    0x004
#define EPOLLERR    0x008
#define EPOLLHUP    0x010
#define EPOLLRDHUP  0x2000
#define EPOLLEXCLUSIVE    (1U << 28)
#define EPOLLWAKEUP       (1U << 29)
#define EPOLLONESHOT      (1U << 30)
#define EPOLLET           (1U << 31)

#define EPOLL_EVS_MASK (EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP | \
                        EPOLLRDHUP | EPOLLEXCLUSIVE | EPOLLWAKEUP | \
                        EPOLLONESHOT | EPOLLET)

typedef struct epoll_item {
    int fd;
    int registered;
    epoll_event_t ev;
} epoll_item_t;

typedef struct {
    int count;
    epoll_item_t items[EPOLL_MAX_FDS];
} epoll_t;

static int epoll_close(vfile_t *vf)
{
    if (vf && vf->priv) kfree(vf->priv);
    return 0;
}

static vfile_ops_t g_epoll_ops = {
    .close = epoll_close,
};

static int vfile_is_epoll(vfile_t *vf)
{
    return vf && vf->ops == &g_epoll_ops;
}

/*
 * Acquire a reference to the epoll instance behind epfd.
 * Returns the epoll_t pointer on success (caller must NOT free it).
 * On failure returns NULL.
 */
static epoll_t *epoll_get(int epfd)
{
    int64_t gfd = fdtable_get_current(epfd);
    if (gfd < 0) return NULL;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return NULL;
    if (!vfile_is_epoll(vf)) {
        vfs_put_file_ref((int)gfd, vf);
        return NULL;
    }
    epoll_t *ep = (epoll_t *)vf->priv;
    vfs_put_file_ref((int)gfd, vf);
    return ep;
}

/*
 * Acquire both the epoll_t pointer AND a reference to the underlying
 * vfile so the caller can safely use ep for an extended period.
 * Caller must call epoll_put_ref() when done.
 */
static epoll_t *epoll_get_ref(int epfd, int *out_gfd, vfile_t **out_vf)
{
    int64_t gfd = fdtable_get_current(epfd);
    if (gfd < 0) return NULL;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return NULL;
    if (!vfile_is_epoll(vf)) {
        vfs_put_file_ref((int)gfd, vf);
        return NULL;
    }
    *out_gfd = (int)gfd;
    *out_vf = vf;
    return (epoll_t *)vf->priv;
}

static void epoll_put_ref(int gfd, vfile_t *vf)
{
    if (vf)
        vfs_put_file_ref(gfd, vf);
}

static int check_epfd(int epfd)
{
    int64_t gfd = fdtable_get_current(epfd);
    if (gfd < 0) return -EBADF;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    int is_ep = vfile_is_epoll(vf);
    vfs_put_file_ref((int)gfd, vf);
    return is_ep ? 0 : -EINVAL;
}

static int epoll_check_cycle(int root_fd, int target_fd, int depth)
{
    if (depth >= EPOLL_MAX_NESTING)
        return -EINVAL;
    if (root_fd == target_fd)
        return -ELOOP;

    epoll_t *target_ep = epoll_get(target_fd);
    if (!target_ep)
        return 0;

    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        if (!target_ep->items[i].registered) continue;
        int watched_fd = target_ep->items[i].fd;
        if (watched_fd == root_fd)
            return -ELOOP;

        int64_t gfd = fdtable_get_current(watched_fd);
        if (gfd < 0) continue;
        vfile_t *vf = vfs_get_file_ref((int)gfd);
        if (!vf) continue;
        int is_ep = vfile_is_epoll(vf);
        vfs_put_file_ref((int)gfd, vf);
        if (!is_ep) continue;

        int r = epoll_check_cycle(root_fd, watched_fd, depth + 1);
        if (r < 0)
            return r;
    }
    return 0;
}

static int epoll_find(epoll_t *ep, int fd)
{
    for (int i = 0; i < EPOLL_MAX_FDS; i++)
        if (ep->items[i].registered && ep->items[i].fd == fd)
            return i;
    return -1;
}

static int epoll_find_free(epoll_t *ep)
{
    for (int i = 0; i < EPOLL_MAX_FDS; i++)
        if (!ep->items[i].registered)
            return i;
    return -1;
}

static short epoll_events_to_poll(uint32_t events)
{
    short pe = 0;
    if (events & EPOLLIN)    pe |= POLLIN;
    if (events & EPOLLOUT)   pe |= POLLOUT;
    if (events & EPOLLERR)   pe |= POLLERR;
    if (events & EPOLLHUP)   pe |= POLLHUP;
    if (events & EPOLLRDHUP) pe |= POLLHUP;
    if (!pe) pe = POLLIN | POLLOUT;
    return pe;
}

static uint32_t poll_events_to_epoll(short pe, uint32_t orig_events)
{
    uint32_t ev = orig_events & ~((uint32_t)(EPOLLIN | EPOLLOUT | EPOLLERR | EPOLLHUP));
    if (pe & POLLIN)  ev |= EPOLLIN;
    if (pe & POLLOUT) ev |= EPOLLOUT;
    if (pe & POLLERR) ev |= EPOLLERR;
    if (pe & POLLHUP) ev |= EPOLLHUP;
    return ev;
}

int64_t sys_epoll_create1(int flags)
{
    if (flags & ~O_CLOEXEC) return -EINVAL;
    vfile_t *vf = vfile_alloc();
    if (!vf) return -ENOMEM;

    epoll_t *ep = (epoll_t *)kmalloc(sizeof(*ep));
    if (!ep) { vfile_free(vf); return -ENOMEM; }
    memset(ep, 0, sizeof(*ep));

    refcount_set(&vf->ref_count, 1);
    vf->priv = ep;
    vf->ops = &g_epoll_ops;

    int gfd = vfs_alloc_fd(vf);
    if (gfd < 0) {
        kfree(ep);
        vfile_free(vf);
        return -EMFILE;
    }
    return fdtable_install_current(gfd, flags);
}

int64_t sys_epoll_create(int size)
{
    if (size <= 0) return -EINVAL;
    return sys_epoll_create1(0);
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, void *event)
{
    int err = check_epfd(epfd);
    if (err < 0) return err;

    epoll_t *ep = epoll_get(epfd);
    if (!ep) return -EBADF;

    if (epfd == fd) return -EINVAL;

    int64_t target_gfd = fdtable_get_current(fd);
    if (target_gfd < 0) return -EBADF;

    if (op != EPOLL_CTL_DEL) {
        if (!event) return -EFAULT;
    }

    epoll_event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (event && copy_from_user(&ev, event, sizeof(ev)) < 0)
        return -EFAULT;

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (epoll_find(ep, fd) >= 0)
            return -EEXIST;

        {
            vfile_t *vf_check = vfs_get_file_ref((int)target_gfd);
            if (vf_check && vfile_is_epoll(vf_check)) {
                int cyc_err = epoll_check_cycle(epfd, fd, 0);
                if (cyc_err < 0) {
                    vfs_put_file_ref((int)target_gfd, vf_check);
                    return cyc_err;
                }
            }
            if (vf_check && vf_check->vnode &&
                vf_check->vnode->type == VFS_FT_DIR) {
                vfs_put_file_ref((int)target_gfd, vf_check);
                return -EPERM;
            }
            if (vf_check) vfs_put_file_ref((int)target_gfd, vf_check);
        }

        int idx = epoll_find_free(ep);
        if (idx < 0) return -ENOSPC;
        ep->items[idx].fd = fd;
        ep->items[idx].ev = ev;
        ep->items[idx].registered = 1;
        ep->count++;
        return 0;
    }

    case EPOLL_CTL_MOD: {
        int idx = epoll_find(ep, fd);
        if (idx < 0)
            return -ENOENT;
        ep->items[idx].ev = ev;
        return 0;
    }

    case EPOLL_CTL_DEL: {
        int idx = epoll_find(ep, fd);
        if (idx < 0)
            return -ENOENT;
        ep->items[idx].registered = 0;
        ep->items[idx].fd = -1;
        ep->count--;
        return 0;
    }

    default:
        return -EINVAL;
    }
}

static int epoll_do_wait(int epfd, void *events, int maxevents,
                         int timeout_ms, const void *sigmask,
                         size_t sigsetsize)
{
    (void)sigsetsize;

    int err = check_epfd(epfd);
    if (err < 0) return err;

    int ep_gfd = -1;
    vfile_t *ep_vf = NULL;
    epoll_t *ep = epoll_get_ref(epfd, &ep_gfd, &ep_vf);
    if (!ep) return -EBADF;
    if (!events || maxevents <= 0 || maxevents > EPOLL_MAX_FDS) {
        epoll_put_ref(ep_gfd, ep_vf);
        return -EINVAL;
    }

    task_t *t = proc_current();

    signal_state_t *saved_ss = NULL;
    uint64_t saved_blocked = 0;
    if (sigmask) {
        if (!t || !t->signals) {
            epoll_put_ref(ep_gfd, ep_vf);
            return -EINVAL;
        }
        uint64_t user_mask;
        if (copy_from_user(&user_mask, sigmask, sizeof(user_mask)) < 0) {
            epoll_put_ref(ep_gfd, ep_vf);
            return -EFAULT;
        }
        saved_ss = (signal_state_t *)t->signals;
        saved_blocked = t->sig_blocked;
        t->sig_blocked = signal_mask_from_user(user_mask) &
            ~(signal_mask_bit(SIGKILL) | signal_mask_bit(SIGSTOP));
    }

    int has_timeout = timeout_ms >= 0;
    uint64_t deadline = 0;
    if (has_timeout) {
        uint64_t ticks = (uint64_t)timeout_ms * TICKS_PER_SEC / 1000ULL;
        deadline = timer_get_ticks() + (ticks ? ticks : 1);
    }

    int total_ready = 0;
    int first_pass = 1;

    for (;;) {
        int n = 0;
        for (int i = 0; i < EPOLL_MAX_FDS && n < maxevents; i++) {
            if (!ep->items[i].registered) continue;

            int fd = ep->items[i].fd;
            int64_t gfd = fdtable_get_current(fd);
            if (gfd < 0) continue;

            short poll_events = epoll_events_to_poll(ep->items[i].ev.events);
            int revents = vfs_poll_events((int)gfd, poll_events);
            if (revents <= 0) continue;

            epoll_event_t out_ev;
            out_ev.events = poll_events_to_epoll((short)revents,
                                                  ep->items[i].ev.events);
            out_ev.data = ep->items[i].ev.data;

            if (copy_to_user((char *)events + (size_t)n * sizeof(epoll_event_t),
                             &out_ev, sizeof(epoll_event_t)) < 0) {
                if (sigmask && saved_ss)
                    t->sig_blocked = saved_blocked;
                epoll_put_ref(ep_gfd, ep_vf);
                return -EFAULT;
            }
            n++;

            if (ep->items[i].ev.events & EPOLLONESHOT) {
                ep->items[i].registered = 0;
                ep->count--;
            }
        }

        if (n > 0) {
            total_ready = n;
            break;
        }

        if (!first_pass) {
            if (has_timeout && timer_get_ticks() >= deadline) {
                total_ready = 0;
                break;
            }
            if (t && signal_task_has_unblocked(t)) {
                if (sigmask && saved_ss) {
                    t->sigsuspend_old_blocked = saved_blocked;
                    t->sigsuspend_active = 1;
                }
                epoll_put_ref(ep_gfd, ep_vf);
                return -EINTR;
            }
        }
        first_pass = 0;

        if (has_timeout && timeout_ms == 0) {
            total_ready = 0;
            break;
        }

        if (has_timeout && timer_get_ticks() >= deadline) {
            total_ready = 0;
            break;
        }

        if (t) {
            uint64_t now = timer_get_ticks();
            uint64_t sleep_until = now + MS_TO_TICKS(20);
            if (has_timeout && deadline < sleep_until)
                sleep_until = deadline;
            proc_block_until(t, sleep_until);
            sched();
            if (t->state == PROC_BLOCKED)
                t->state = PROC_RUNNING;
            proc_set_wake_time(t, 0);
        } else {
            /*
             * No task context — cannot sleep.  For infinite timeout
             * this would busy-loop forever, so just return 0.
             */
            if (!has_timeout)
                break;
        }
    }

    if (sigmask && saved_ss)
        t->sig_blocked = saved_blocked;

    epoll_put_ref(ep_gfd, ep_vf);
    return total_ready;
}

int64_t sys_epoll_wait(int epfd, void *events, int maxevents, int timeout)
{
    return epoll_do_wait(epfd, events, maxevents, timeout, NULL, 0);
}

int64_t sys_epoll_pwait(int epfd, void *events, int maxevents,
                        int timeout, const void *sigmask, size_t sigsetsize)
{
    return epoll_do_wait(epfd, events, maxevents, timeout, sigmask, sigsetsize);
}

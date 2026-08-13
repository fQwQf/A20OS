#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "proc/proc_internal.h"
#include "fs/vfs/file.h"
#include "fs/readiness.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"

typedef struct {
    uint32_t events;
    uint64_t data;
}
#if defined(CONFIG_X86_64)
__attribute__((packed))
#endif
epoll_event_t;

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
    uint64_t identity;
    epoll_event_t ev;
    readiness_state_t state;
} epoll_item_t;

typedef struct {
    spinlock_t lock;
    int count;
    wait_queue_t control_waiters;
    uint64_t change_seq;
    epoll_item_t items[EPOLL_MAX_FDS];
} epoll_t;

typedef struct {
    int index;
    int fd;
    uint64_t identity;
    epoll_event_t ev;
    readiness_state_t state;
} epoll_wait_item_t;

static short epoll_events_to_poll(uint32_t events);
static uint32_t poll_events_to_epoll(short pe, uint32_t orig_events);

static int epoll_poll(vfile_t *vf, short events)
{
    epoll_t *ep = vf ? vf->priv : NULL;
    if (!ep) return POLLNVAL;
    if (!(events & POLLIN)) return 0;

    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        if (!ep->items[i].registered || !ep->items[i].state.enabled) {
            spin_unlock_irqrestore(&ep->lock, flags);
            continue;
        }
        int fd = ep->items[i].fd;
        uint64_t identity = ep->items[i].identity;
        epoll_event_t ev = ep->items[i].ev;
        spin_unlock_irqrestore(&ep->lock, flags);

        int gfd = -1;
        vfile_t *target = fdtable_get_current_file_ref(fd, &gfd);
        if (!target)
            continue;
        short requested = epoll_events_to_poll(ev.events);
        int ready = target->identity == identity ?
                    vfs_poll_file(target, requested) : 0;
        vfs_put_file_ref(gfd, target);
        if (ready > 0 &&
            poll_events_to_epoll((short)ready, ev.events))
            return POLLIN;
    }
    return 0;
}

static int epoll_close(vfile_t *vf)
{
    if (vf && vf->priv) kfree(vf->priv);
    return 0;
}

static vfile_ops_t g_epoll_ops = {
    .poll = epoll_poll,
    .close = epoll_close,
};

static int vfile_is_epoll(vfile_t *vf)
{
    return vf && vf->ops == &g_epoll_ops;
}

/*
 * Acquire both the epoll_t pointer AND a reference to the underlying
 * vfile so the caller can safely use ep for an extended period.
 * Caller must call epoll_put_ref() when done.
 */
static epoll_t *epoll_get_ref(int epfd, int *out_gfd, vfile_t **out_vf)
{
    int gfd = -1;
    vfile_t *vf = fdtable_get_current_file_ref(epfd, &gfd);
    if (!vf) return NULL;
    if (!vfile_is_epoll(vf)) {
        vfs_put_file_ref((int)gfd, vf);
        return NULL;
    }
    *out_gfd = gfd;
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

    int target_gfd = -1;
    vfile_t *target_vf = NULL;
    epoll_t *target_ep = epoll_get_ref(target_fd, &target_gfd, &target_vf);
    if (!target_ep)
        return 0;

    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        uint64_t flags = spin_lock_irqsave(&target_ep->lock);
        int registered = target_ep->items[i].registered;
        int watched_fd = target_ep->items[i].fd;
        spin_unlock_irqrestore(&target_ep->lock, flags);
        if (!registered)
            continue;
        if (watched_fd == root_fd) {
            epoll_put_ref(target_gfd, target_vf);
            return -ELOOP;
        }

        int64_t gfd = fdtable_get_current(watched_fd);
        if (gfd < 0) continue;
        vfile_t *vf = vfs_get_file_ref((int)gfd);
        if (!vf) continue;
        int is_ep = vfile_is_epoll(vf);
        vfs_put_file_ref((int)gfd, vf);
        if (!is_ep) continue;

        int r = epoll_check_cycle(root_fd, watched_fd, depth + 1);
        if (r < 0) {
            epoll_put_ref(target_gfd, target_vf);
            return r;
        }
    }
    epoll_put_ref(target_gfd, target_vf);
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
    uint32_t ev = 0;
    if (pe & POLLIN)  ev |= EPOLLIN;
    if (pe & POLLOUT) ev |= EPOLLOUT;
    if (pe & POLLERR) ev |= EPOLLERR;
    if (pe & POLLHUP) ev |= EPOLLHUP;
    if ((pe & POLLHUP) && (orig_events & EPOLLRDHUP))
        ev |= EPOLLRDHUP;
    return ev;
}

static uint32_t epoll_readiness_mode(uint32_t events)
{
    uint32_t mode = 0;
    if (events & EPOLLET)
        mode |= READINESS_MODE_EDGE;
    if (events & EPOLLONESHOT)
        mode |= READINESS_MODE_ONESHOT;
    return mode;
}

static void epoll_change_locked(epoll_t *ep)
{
    __atomic_add_fetch(&ep->change_seq, 1, __ATOMIC_RELEASE);
}

static void epoll_wake_changed(epoll_t *ep)
{
    wait_queue_wake_all(&ep->control_waiters, 0, PROC_WAKE_EVENT);
}

typedef struct {
    epoll_t *ep;
    uint64_t sequence;
} epoll_change_probe_t;

static bool epoll_change_pending(void *arg)
{
    epoll_change_probe_t *probe = arg;
    return __atomic_load_n(&probe->ep->change_seq, __ATOMIC_ACQUIRE) !=
           probe->sequence;
}

int64_t sys_epoll_create1(int flags)
{
    if (flags & ~O_CLOEXEC) return -EINVAL;
    vfile_t *vf = vfile_alloc();
    if (!vf) return -ENOMEM;

    epoll_t *ep = (epoll_t *)kmalloc(sizeof(*ep));
    if (!ep) { vfile_free(vf); return -ENOMEM; }
    memset(ep, 0, sizeof(*ep));
    spin_init(&ep->lock);
    wait_queue_init(&ep->control_waiters);

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

    int ep_gfd = -1;
    vfile_t *ep_vf = NULL;
    epoll_t *ep = epoll_get_ref(epfd, &ep_gfd, &ep_vf);
    if (!ep) return -EBADF;
#define EPOLL_CTL_RETURN(v) do { epoll_put_ref(ep_gfd, ep_vf); return (v); } while (0)

    if (epfd == fd) EPOLL_CTL_RETURN(-EINVAL);

    int target_gfd = -1;
    vfile_t *target_vf = fdtable_get_current_file_ref(fd, &target_gfd);
    if (!target_vf) EPOLL_CTL_RETURN(-EBADF);
    uint64_t target_identity = target_vf->identity;
#undef EPOLL_CTL_RETURN
#define EPOLL_CTL_RETURN(v) do { \
    vfs_put_file_ref(target_gfd, target_vf); \
    epoll_put_ref(ep_gfd, ep_vf); \
    return (v); \
} while (0)

    if (op != EPOLL_CTL_DEL) {
        if (!event) EPOLL_CTL_RETURN(-EFAULT);
    }

    epoll_event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (event && copy_from_user(&ev, event, sizeof(ev)) < 0)
        EPOLL_CTL_RETURN(-EFAULT);

    switch (op) {
    case EPOLL_CTL_ADD: {
        if (vfile_is_epoll(target_vf)) {
            int cyc_err = epoll_check_cycle(epfd, fd, 0);
            if (cyc_err < 0)
                EPOLL_CTL_RETURN(cyc_err);
        }
        if (target_vf->vnode && target_vf->vnode->type == VFS_FT_DIR)
            EPOLL_CTL_RETURN(-EPERM);

        uint64_t flags = spin_lock_irqsave(&ep->lock);
        int idx = epoll_find(ep, fd);
        if (idx >= 0 && ep->items[idx].identity == target_identity) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-EEXIST);
        }
        if (idx < 0)
            idx = epoll_find_free(ep);
        if (idx < 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-ENOSPC);
        }
        bool replacing = ep->items[idx].registered;
        ep->items[idx].fd = fd;
        ep->items[idx].identity = target_identity;
        ep->items[idx].ev = ev;
        ep->items[idx].registered = 1;
        readiness_state_init(&ep->items[idx].state,
                             epoll_readiness_mode(ev.events));
        if (!replacing)
            ep->count++;
        epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, flags);
        epoll_wake_changed(ep);
        EPOLL_CTL_RETURN(0);
    }

    case EPOLL_CTL_MOD: {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        int idx = epoll_find(ep, fd);
        if (idx < 0 || ep->items[idx].identity != target_identity) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-ENOENT);
        }
        ep->items[idx].ev = ev;
        readiness_state_rearm(&ep->items[idx].state,
                              epoll_readiness_mode(ev.events));
        epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, flags);
        epoll_wake_changed(ep);
        EPOLL_CTL_RETURN(0);
    }

    case EPOLL_CTL_DEL: {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        int idx = epoll_find(ep, fd);
        if (idx < 0 || ep->items[idx].identity != target_identity) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-ENOENT);
        }
        ep->items[idx].registered = 0;
        ep->items[idx].fd = -1;
        ep->items[idx].identity = 0;
        ep->count--;
        epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, flags);
        epoll_wake_changed(ep);
        EPOLL_CTL_RETURN(0);
    }

    default:
        EPOLL_CTL_RETURN(-EINVAL);
    }
#undef EPOLL_CTL_RETURN
}

static int epoll_do_wait(int epfd, void *events, int maxevents,
                         int timeout_ms, const void *sigmask,
                         size_t sigsetsize)
{
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
        if (sigsetsize != ARCH_SIGSET_SIZE) {
            epoll_put_ref(ep_gfd, ep_vf);
            return -EINVAL;
        }
        if (!t || !t->signals) {
            epoll_put_ref(ep_gfd, ep_vf);
            return -EINVAL;
        }
        arch_sigset_t user_mask;
        if (copy_from_user(&user_mask, sigmask, sizeof(user_mask)) < 0) {
            epoll_put_ref(ep_gfd, ep_vf);
            return -EFAULT;
        }
        saved_ss = (signal_state_t *)t->signals;
        int mask_ret = signal_task_set_temporary_mask(
            t, arch_user_sigset_to_kernel(&user_mask), &saved_blocked);
        if (mask_ret < 0) {
            epoll_put_ref(ep_gfd, ep_vf);
            return mask_ret;
        }
    }

    int has_timeout = timeout_ms >= 0;
    uint64_t deadline = 0;
    if (has_timeout) {
        uint64_t ticks = timeout_ms > 0 ?
            (uint64_t)timeout_ms * TICKS_PER_SEC / 1000ULL : 0;
        if (timeout_ms > 0 && ticks == 0)
            ticks = 1;
        deadline = timer_get_ticks() + ticks;
    }

    readiness_interest_t *interests =
        kcalloc(EPOLL_MAX_FDS, sizeof(*interests));
    epoll_wait_item_t *snapshots =
        kcalloc(EPOLL_MAX_FDS, sizeof(*snapshots));
    if (!interests || !snapshots) {
        if (sigmask && saved_ss)
            signal_task_restore_mask(t, saved_blocked);
        kfree(interests);
        kfree(snapshots);
        epoll_put_ref(ep_gfd, ep_vf);
        return -ENOMEM;
    }

    int total_ready = 0;
    for (;;) {
        memset(interests, 0, EPOLL_MAX_FDS * sizeof(*interests));
        memset(snapshots, 0, EPOLL_MAX_FDS * sizeof(*snapshots));
        size_t interest_count = 0;
        uint64_t lock_flags = spin_lock_irqsave(&ep->lock);
        epoll_change_probe_t probe = { .ep = ep, .sequence = ep->change_seq };
        for (int i = 0; i < EPOLL_MAX_FDS; i++) {
            if (!ep->items[i].registered || !ep->items[i].state.enabled)
                continue;
            snapshots[interest_count] = (epoll_wait_item_t){
                .index = i,
                .fd = ep->items[i].fd,
                .identity = ep->items[i].identity,
                .ev = ep->items[i].ev,
                .state = ep->items[i].state,
            };
            interests[interest_count++] = (readiness_interest_t){
                .fd = ep->items[i].fd,
                .events = epoll_events_to_poll(ep->items[i].ev.events),
                .cookie = interest_count - 1,
                .expected_identity = ep->items[i].identity,
                .state = &snapshots[interest_count - 1].state,
            };
        }
        spin_unlock_irqrestore(&ep->lock, lock_flags);
        readiness_extra_t control = {
            .source = { &ep->control_waiters, 0, 0 },
            .ready = epoll_change_pending,
            .arg = &probe,
        };
        int wait_result = readiness_wait_once(
            interests, interest_count, &control, 1, (size_t)maxevents,
            deadline, has_timeout);

        bool pruned = false;
        lock_flags = spin_lock_irqsave(&ep->lock);
        if (ep->change_seq != probe.sequence) {
            spin_unlock_irqrestore(&ep->lock, lock_flags);
            continue;
        }
        for (size_t j = 0; j < interest_count; j++) {
            epoll_wait_item_t *snapshot = &snapshots[j];
            epoll_item_t *item = &ep->items[snapshot->index];
            if (!item->registered || item->fd != snapshot->fd ||
                item->identity != snapshot->identity)
                continue;
            item->state = snapshot->state;
            if (interests[j].revents & POLLNVAL) {
                item->registered = 0;
                item->fd = -1;
                item->identity = 0;
                ep->count--;
                pruned = true;
            }
        }
        if (pruned)
            epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, lock_flags);
        if (pruned)
            epoll_wake_changed(ep);

        if (wait_result == READINESS_RETRY)
            continue;
        if (wait_result < 0) {
            total_ready = wait_result;
            break;
        }
        if (wait_result == 0) {
            total_ready = 0;
            break;
        }

        int n = 0;
        for (size_t j = 0; j < interest_count && n < maxevents; j++) {
            readiness_interest_t *interest = &interests[j];
            if (!interest->revents)
                continue;
            if (interest->revents & POLLNVAL)
                continue;
            epoll_wait_item_t *snapshot =
                &snapshots[(size_t)interest->cookie];
            epoll_event_t out_ev;
            out_ev.events = poll_events_to_epoll(interest->revents,
                                                  snapshot->ev.events);
            if (!out_ev.events)
                continue;
            out_ev.data = snapshot->ev.data;

            if (copy_to_user((char *)events + (size_t)n * sizeof(epoll_event_t),
                             &out_ev, sizeof(epoll_event_t)) < 0) {
                if (sigmask && saved_ss)
                    signal_task_restore_mask(t, saved_blocked);
                kfree(interests);
                kfree(snapshots);
                epoll_put_ref(ep_gfd, ep_vf);
                return -EFAULT;
            }
            n++;
        }
        if (n > 0) {
            total_ready = n;
            break;
        }
    }

    if (sigmask && saved_ss) {
        if (total_ready == -EINTR)
            signal_task_defer_mask_restore(t, saved_blocked);
        else
            signal_task_restore_mask(t, saved_blocked);
    }

    kfree(interests);
    kfree(snapshots);
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

struct linux_timespec64_epoll {
    int64_t tv_sec;
    int64_t tv_nsec;
};

int64_t sys_epoll_pwait2(int epfd, void *events, int maxevents,
                         const void *timeout, const void *sigmask,
                         size_t sigsetsize)
{
    int timeout_ms = -1;
    if (timeout) {
        struct linux_timespec64_epoll ts;
        if (copy_from_user(&ts, timeout, sizeof(ts)) < 0)
            return -EFAULT;
        if (ts.tv_sec < 0 || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000LL)
            return -EINVAL;
        uint64_t total_ns = (uint64_t)ts.tv_sec * 1000000000ULL +
                            (uint64_t)ts.tv_nsec;
        uint64_t ms = total_ns / 1000000ULL;
        timeout_ms = ms > (uint64_t)0x7fffffff ? 0x7fffffff : (int)ms;
    }
    return epoll_do_wait(epfd, events, maxevents, timeout_ms, sigmask,
                         sigsetsize);
}

#include "fs/eventpoll.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/string.h"
#include "sys/usercopy.h"
#include "proc/proc_internal.h"
#include "fs/vfs/file.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "fs/readiness.h"
#include "core/timer.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "proc/signal.h"


#define EPOLL_MAX_FDS  1024
#define EPOLL_MAX_NESTING 4

typedef struct epoll_item {
    int fd;
    int registered;
    uint64_t identity;
    struct eventpoll_event ev;
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
    struct eventpoll_event ev;
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
        struct eventpoll_event ev = ep->items[i].ev;
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

int epoll_slot_contains_file(struct task_t *owner, struct vfile *epoll_vf,
                             uint64_t file_identity)
{
    if (!owner || !epoll_vf || epoll_vf->ops != &g_epoll_ops ||
        !epoll_vf->priv)
        return -EBADF;
    epoll_t *ep = epoll_vf->priv;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        int registered = ep->items[i].registered;
        int fd = ep->items[i].fd;
        spin_unlock_irqrestore(&ep->lock, flags);
        if (!registered)
            continue;
        int gfd = fdtable_get(owner, fd);
        if (gfd < 0)
            continue;
        vfile_t *vf = vfs_get_file_ref(gfd);
        if (!vf)
            continue;
        int match = vf->identity == file_identity;
        vfs_put_file_ref(gfd, vf);
        if (match)
            return 1;
    }
    return 0;
}

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

int eventpoll_create(int flags)
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

int eventpoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data)
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


    struct eventpoll_event ev = { .events = events, .data = data };

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

int eventpoll_wait(int epfd, struct eventpoll_event *out_events,
                   int maxevents, int timeout_ms,
                   const void *sigmask, size_t sigsetsize)
{
    int ep_gfd = -1;
    vfile_t *ep_vf = NULL;
    epoll_t *ep = epoll_get_ref(epfd, &ep_gfd, &ep_vf);
    if (!ep) return -EBADF;
    if (!out_events || maxevents <= 0 || maxevents > EPOLL_MAX_FDS) {
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
            struct eventpoll_event out_ev;
            out_ev.events = poll_events_to_epoll(interest->revents,
                                                  snapshot->ev.events);
            if (!out_ev.events)
                continue;
            out_ev.data = snapshot->ev.data;
            out_events[n++] = out_ev;
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

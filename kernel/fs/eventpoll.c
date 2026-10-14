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

/* Serialize graph validation with every ctl edge mutation. */
static spinlock_t g_epoll_graph_lock = SPINLOCK_INIT;

typedef struct epoll epoll_t;
static epoll_t *g_epoll_registry;
static size_t g_epoll_watch_count;
static void epoll_registry_remove_locked(epoll_t *ep);

typedef struct epoll_item {
    int fd;
    int registered;
    uint64_t identity;
    struct eventpoll_event ev;
    readiness_state_t state;
} epoll_item_t;

struct epoll {
    spinlock_t lock;
    int count;
    wait_queue_t control_waiters;
    uint64_t change_seq;
    epoll_item_t items[EPOLL_MAX_FDS];
    vfile_t *targets[EPOLL_MAX_FDS];
    epoll_t *registry_next;
    bool in_registry;
};

typedef struct {
    int index;
    int fd;
    uint64_t identity;
    struct eventpoll_event ev;
    readiness_state_t state;
    vfile_t *target;
    int pruned;
} epoll_wait_item_t;

#ifdef CONFIG_EVENTPOLL_STATE_RUST
int a20_eventpoll_find_rust(void *items, int fd, uint64_t identity);
int a20_eventpoll_find_free_rust(void *items);
int a20_eventpoll_add_rust(void *items, int *count, int fd,
                           uint64_t identity, uint32_t events, uint64_t data);
int a20_eventpoll_mod_rust(void *items, int fd, uint64_t identity,
                           uint32_t events, uint64_t data);
int a20_eventpoll_del_rust(void *items, int *count, int fd, uint64_t identity);
int a20_eventpoll_del_index_rust(void *items, int *count, size_t index,
                                 int fd, uint64_t identity);
bool a20_eventpoll_commit_rust(void *items, int *count, size_t index, int fd,
                               uint64_t identity, const void *state,
                               bool invalid);
short a20_eventpoll_filter_ready_rust(short raw, uint64_t generation,
                                      void *state);

static short epoll_filter_ready_rust(short raw, uint64_t generation,
                                     void *state)
{
    return a20_eventpoll_filter_ready_rust(raw, generation, state);
}
#endif

/* Defined by fdtable; this scan must be made without the epoll lock held. */
bool fdtable_file_has_open_slot(vfile_t *vf);

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
        uint64_t identity = ep->items[i].identity;
        struct eventpoll_event ev = ep->items[i].ev;
        vfile_t *target = ep->targets[i];
        if (target)
            vfile_get(target);
        spin_unlock_irqrestore(&ep->lock, flags);

        if (!target)
            continue;
        if (!fdtable_file_has_open_slot(target)) {
            vfs_put_file(target);
            continue;
        }
        short requested = epoll_events_to_poll(ev.events);
        int ready = target->identity == identity ?
                    vfs_poll_file(target, requested) : 0;
        vfs_put_file(target);
        if (ready > 0 &&
            poll_events_to_epoll((short)ready, ev.events))
            return POLLIN;
    }
    return 0;
}

static int epoll_close(vfile_t *vf)
{
    epoll_t *ep = vf ? vf->priv : NULL;
    if (ep) {
        spin_lock(&g_epoll_graph_lock);
        epoll_registry_remove_locked(ep);
        __atomic_sub_fetch(&g_epoll_watch_count, (size_t)ep->count,
                           __ATOMIC_RELEASE);
        spin_unlock(&g_epoll_graph_lock);
        for (int i = 0; i < EPOLL_MAX_FDS; i++)
            if (ep->targets[i])
                vfs_put_file(ep->targets[i]);
        kfree(ep);
    }
    return 0;
}

static size_t epoll_poll_sources(vfile_t *vf, short events,
                                 readiness_source_t *sources, size_t max)
{
    epoll_t *ep = vf ? vf->priv : NULL;
    if (!ep || !sources || max == 0 || !(events & POLLIN))
        return 0;

    size_t count = 0;
    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        if (!ep->items[i].registered || !ep->items[i].state.enabled) {
            spin_unlock_irqrestore(&ep->lock, flags);
            continue;
        }
        uint64_t identity = ep->items[i].identity;
        struct eventpoll_event ev = ep->items[i].ev;
        vfile_t *target = ep->targets[i];
        if (target)
            vfile_get(target);
        spin_unlock_irqrestore(&ep->lock, flags);

        if (!target)
            continue;
        if (!fdtable_file_has_open_slot(target)) {
            vfs_put_file(target);
            continue;
        }
        bool pollable = target->identity == identity && target->ops &&
                        target->ops->poll_sources;
        size_t n = 0;
        if (pollable)
            n = target->ops->poll_sources(
                target, epoll_events_to_poll(ev.events),
                count < max ? sources + count : NULL,
                count < max ? max - count : 0);
        vfs_put_file(target);
        if (!pollable || count + n > max)
            return 0;
        count += n;
    }
    return count;
}

static vfile_ops_t g_epoll_ops = {
    .poll = epoll_poll,
    .poll_sources = epoll_poll_sources,
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
        int match = registered && ep->items[i].identity == file_identity;
        spin_unlock_irqrestore(&ep->lock, flags);
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
static epoll_t *epoll_get_ref(int epfd, vfile_t **out_vf)
{
    vfile_t *vf = fdtable_get_current_file_ref(epfd);
    if (!vf) return NULL;
    if (!vfile_is_epoll(vf)) {
        vfs_put_file(vf);
        return NULL;
    }
    *out_vf = vf;
    return (epoll_t *)vf->priv;
}

static void epoll_put_ref(vfile_t *vf)
{
    if (vf)
        vfs_put_file(vf);
}

static int check_epfd(int epfd)
{
    vfile_t *vf = fdtable_get_current_file_ref(epfd);
    if (!vf) return -EBADF;
    int is_ep = vfile_is_epoll(vf);
    vfs_put_file(vf);
    return is_ep ? 0 : -EINVAL;
}

static int epoll_check_cycle(uint64_t root_identity, vfile_t *target_vf,
                             int depth)
{
    if (depth >= EPOLL_MAX_NESTING)
        return -EINVAL;
    if (!target_vf)
        return 0;
    if (target_vf->identity == root_identity)
        return -ELOOP;
    if (!vfile_is_epoll(target_vf) || !target_vf->priv)
        return 0;
    epoll_t *target_ep = target_vf->priv;

    for (int i = 0; i < EPOLL_MAX_FDS; i++) {
        uint64_t flags = spin_lock_irqsave(&target_ep->lock);
        int registered = target_ep->items[i].registered;
        vfile_t *watched = registered ? target_ep->targets[i] : NULL;
        if (watched)
            vfile_get(watched);
        spin_unlock_irqrestore(&target_ep->lock, flags);
        if (!watched)
            continue;
        int r = epoll_check_cycle(root_identity, watched, depth + 1);
        vfs_put_file(watched);
        if (r < 0) {
            return r;
        }
    }
    return 0;
}

static int epoll_find(epoll_t *ep, int fd, uint64_t identity)
{
#ifdef CONFIG_EVENTPOLL_STATE_RUST
    return a20_eventpoll_find_rust(ep->items, fd, identity);
#else
    for (int i = 0; i < EPOLL_MAX_FDS; i++)
        if (ep->items[i].registered && ep->items[i].fd == fd &&
            ep->items[i].identity == identity)
            return i;
    return -1;
#endif
}

static int epoll_find_free(epoll_t *ep)
{
#ifdef CONFIG_EVENTPOLL_STATE_RUST
    return a20_eventpoll_find_free_rust(ep->items);
#else
    for (int i = 0; i < EPOLL_MAX_FDS; i++)
        if (!ep->items[i].registered)
            return i;
    return -1;
#endif
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

#ifndef CONFIG_EVENTPOLL_STATE_RUST
static uint32_t epoll_readiness_mode(uint32_t events)
{
    uint32_t mode = 0;
    if (events & EPOLLET)
        mode |= READINESS_MODE_EDGE;
    if (events & EPOLLONESHOT)
        mode |= READINESS_MODE_ONESHOT;
    return mode;
}
#endif

static void epoll_change_locked(epoll_t *ep)
{
    __atomic_add_fetch(&ep->change_seq, 1, __ATOMIC_RELEASE);
}

static void epoll_wake_changed(epoll_t *ep)
{
    wait_queue_wake_all(&ep->control_waiters, 0, PROC_WAKE_EVENT);
}

static void epoll_registry_add_locked(epoll_t *ep)
{
    ep->registry_next = g_epoll_registry;
    g_epoll_registry = ep;
    ep->in_registry = true;
}

static void epoll_registry_remove_locked(epoll_t *ep)
{
    if (!ep->in_registry)
        return;
    epoll_t **cursor = &g_epoll_registry;
    while (*cursor && *cursor != ep)
        cursor = &(*cursor)->registry_next;
    if (*cursor == ep)
        *cursor = ep->registry_next;
    ep->registry_next = NULL;
    ep->in_registry = false;
}

void eventpoll_file_final_slot_closed(vfile_t *vf)
{
    if (!vf)
        return;

    /* Detach one edge at a time to avoid a dynamically sized scratch array.
     * Release vfile references only after both locks have been dropped. */
    for (;;) {
        epoll_t *changed_ep = NULL;
        vfile_t *release = NULL;
        spin_lock(&g_epoll_graph_lock);
        /* A concurrent dup/install may have acquired a descriptor slot after
         * fdtable's first scan. ADD also revalidates under this graph lock,
         * so this check linearizes final-close pruning against registration. */
        if (fdtable_file_has_open_slot(vf)) {
            spin_unlock(&g_epoll_graph_lock);
            break;
        }
        for (epoll_t *ep = g_epoll_registry; ep && !release;
             ep = ep->registry_next) {
            uint64_t flags = spin_lock_irqsave(&ep->lock);
            for (int i = 0; i < EPOLL_MAX_FDS; i++) {
                epoll_item_t *item = &ep->items[i];
                if (!item->registered || item->identity != vf->identity ||
                    ep->targets[i] != vf)
                    continue;
#ifdef CONFIG_EVENTPOLL_STATE_RUST
                if (a20_eventpoll_del_index_rust(
                        ep->items, &ep->count, (size_t)i, item->fd,
                        item->identity) < 0)
                    continue;
#else
                item->registered = 0;
                item->fd = -1;
                item->identity = 0;
                ep->count--;
#endif
                release = ep->targets[i];
                ep->targets[i] = NULL;
                __atomic_sub_fetch(&g_epoll_watch_count, 1,
                                   __ATOMIC_RELEASE);
                epoll_change_locked(ep);
                changed_ep = ep;
                break;
            }
            spin_unlock_irqrestore(&ep->lock, flags);
        }
        if (changed_ep)
            epoll_wake_changed(changed_ep);
        spin_unlock(&g_epoll_graph_lock);
        if (!release)
            break;
        vfs_put_file(release);
    }
}

int eventpoll_file_has_watch(vfile_t *vf)
{
    if (!vf)
        return false;
    if (__atomic_load_n(&g_epoll_watch_count, __ATOMIC_ACQUIRE) == 0)
        return false;

    bool found = false;
    spin_lock(&g_epoll_graph_lock);
    for (epoll_t *ep = g_epoll_registry; ep && !found;
         ep = ep->registry_next) {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        for (int i = 0; i < EPOLL_MAX_FDS; i++) {
            if (ep->items[i].registered && ep->items[i].identity == vf->identity &&
                ep->targets[i] == vf) {
                found = true;
                break;
            }
        }
        spin_unlock_irqrestore(&ep->lock, flags);
    }
    spin_unlock(&g_epoll_graph_lock);
    return found;
}

typedef struct {
    epoll_t *ep;
    uint64_t sequence;
    epoll_wait_item_t *snapshots;
    size_t count;
} epoll_change_probe_t;

static bool epoll_change_pending(void *arg)
{
    epoll_change_probe_t *probe = arg;
    if (__atomic_load_n(&probe->ep->change_seq, __ATOMIC_ACQUIRE) !=
        probe->sequence)
        return true;
    for (size_t i = 0; i < probe->count; i++) {
        vfile_t *target = probe->snapshots[i].target;
        if (!target || !fdtable_file_has_open_slot(target))
            return true;
    }
    return false;
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
    spin_lock(&g_epoll_graph_lock);
    epoll_registry_add_locked(ep);
    spin_unlock(&g_epoll_graph_lock);

    int fd = fdtable_install_current_vfile(vf, flags);
    if (fd < 0) {
        spin_lock(&g_epoll_graph_lock);
        epoll_registry_remove_locked(ep);
        spin_unlock(&g_epoll_graph_lock);
        kfree(ep);
        vfile_free(vf);
        return fd;
    }
    return fd;
}

int eventpoll_ctl(int epfd, int op, int fd, uint32_t events, uint64_t data)
{
    int err = check_epfd(epfd);
    if (err < 0) return err;

    vfile_t *ep_vf = NULL;
    epoll_t *ep = epoll_get_ref(epfd, &ep_vf);
    if (!ep) return -EBADF;
    bool graph_lock_held = false;
#define EPOLL_CTL_RETURN(v) do { \
    if (graph_lock_held) spin_unlock(&g_epoll_graph_lock); \
    epoll_put_ref(ep_vf); \
    return (v); \
} while (0)

    if (epfd == fd) EPOLL_CTL_RETURN(-EINVAL);

    vfile_t *target_vf = fdtable_get_current_file_ref(fd);
    if (!target_vf) EPOLL_CTL_RETURN(-EBADF);
    uint64_t target_identity = target_vf->identity;
#undef EPOLL_CTL_RETURN
#define EPOLL_CTL_RETURN(v) do { \
    if (graph_lock_held) spin_unlock(&g_epoll_graph_lock); \
    vfs_put_file(target_vf); \
    epoll_put_ref(ep_vf); \
    return (v); \
} while (0)


    struct eventpoll_event ev = { .events = events, .data = data };
#ifdef CONFIG_EVENTPOLL_STATE_RUST
    (void)ev;
#endif

    spin_lock(&g_epoll_graph_lock);
    graph_lock_held = true;
    switch (op) {
    case EPOLL_CTL_ADD: {
        /* Order a racing final-slot close with registration. If the close
         * callback ran first this rejects a stale pin; if it runs afterward,
         * the graph lock makes that callback prune the newly installed edge. */
        if (!fdtable_file_has_open_slot(target_vf))
            EPOLL_CTL_RETURN(-EBADF);
        if (vfile_is_epoll(target_vf)) {
            int cyc_err = epoll_check_cycle(ep_vf->identity, target_vf, 0);
            if (cyc_err < 0)
                EPOLL_CTL_RETURN(cyc_err);
        }
        if (target_vf->vnode && target_vf->vnode->type == VFS_FT_DIR)
            EPOLL_CTL_RETURN(-EPERM);
        /* Linux file_can_poll(): files without a real ->poll (regular,
         * procfs, sysfs, cgroup) cannot join an epoll set.  Accepting them
         * would hand the event loop an always-ready fd and spin it. */
        if (!vfs_file_is_pollable(target_vf))
            EPOLL_CTL_RETURN(-EPERM);

        uint64_t flags = spin_lock_irqsave(&ep->lock);
        int idx = epoll_find(ep, fd, target_identity);
        if (idx >= 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-EEXIST);
        }
        idx = epoll_find_free(ep);
        if (idx < 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-ENOSPC);
        }
        vfile_t *old_target = NULL;
#ifdef CONFIG_EVENTPOLL_STATE_RUST
        int state_ret = a20_eventpoll_add_rust(
            ep->items, &ep->count, fd, target_identity, events, data);
        if (state_ret < 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(state_ret);
        }
#else
        ep->items[idx].fd = fd;
        ep->items[idx].identity = target_identity;
        ep->items[idx].ev = ev;
        ep->items[idx].registered = 1;
        readiness_state_init(&ep->items[idx].state,
                             epoll_readiness_mode(ev.events));
        ep->count++;
#endif
        vfile_get(target_vf);
        ep->targets[idx] = target_vf;
        __atomic_add_fetch(&g_epoll_watch_count, 1, __ATOMIC_RELEASE);
        epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, flags);
        spin_unlock(&g_epoll_graph_lock);
        graph_lock_held = false;
        if (old_target)
            vfs_put_file(old_target);
        epoll_wake_changed(ep);
        EPOLL_CTL_RETURN(0);
    }

    case EPOLL_CTL_MOD: {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        int idx = epoll_find(ep, fd, target_identity);
        if (idx < 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-ENOENT);
        }
#ifdef CONFIG_EVENTPOLL_STATE_RUST
        int state_ret = a20_eventpoll_mod_rust(
            ep->items, fd, target_identity, events, data);
        if (state_ret < 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(state_ret);
        }
#else
        ep->items[idx].ev = ev;
        readiness_state_rearm(&ep->items[idx].state,
                              epoll_readiness_mode(ev.events));
#endif
        epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, flags);
        spin_unlock(&g_epoll_graph_lock);
        graph_lock_held = false;
        epoll_wake_changed(ep);
        EPOLL_CTL_RETURN(0);
    }

    case EPOLL_CTL_DEL: {
        uint64_t flags = spin_lock_irqsave(&ep->lock);
        int idx = epoll_find(ep, fd, target_identity);
        if (idx < 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(-ENOENT);
        }
        vfile_t *old_target = ep->targets[idx];
#ifdef CONFIG_EVENTPOLL_STATE_RUST
        int state_ret = a20_eventpoll_del_index_rust(
            ep->items, &ep->count, (size_t)idx, fd, target_identity);
        if (state_ret < 0) {
            spin_unlock_irqrestore(&ep->lock, flags);
            EPOLL_CTL_RETURN(state_ret);
        }
#else
        ep->items[idx].registered = 0;
        ep->items[idx].fd = -1;
        ep->items[idx].identity = 0;
        ep->count--;
#endif
        ep->targets[idx] = NULL;
        __atomic_sub_fetch(&g_epoll_watch_count, 1, __ATOMIC_RELEASE);
        epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, flags);
        spin_unlock(&g_epoll_graph_lock);
        graph_lock_held = false;
        if (old_target)
            vfs_put_file(old_target);
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
    vfile_t *ep_vf = NULL;
    epoll_t *ep = epoll_get_ref(epfd, &ep_vf);
    if (!ep) return -EBADF;
    if (!out_events || maxevents <= 0 || maxevents > EPOLL_MAX_FDS) {
        epoll_put_ref(ep_vf);
        return -EINVAL;
    }

    task_t *t = proc_current();

    signal_state_t *saved_ss = NULL;
    uint64_t saved_blocked = 0;
    if (sigmask) {
        if (sigsetsize != ARCH_SIGSET_SIZE) {
            epoll_put_ref(ep_vf);
            return -EINVAL;
        }
        if (!t || !t->signals) {
            epoll_put_ref(ep_vf);
            return -EINVAL;
        }
        arch_sigset_t user_mask;
        if (copy_from_user(&user_mask, sigmask, sizeof(user_mask)) < 0) {
            epoll_put_ref(ep_vf);
            return -EFAULT;
        }
        saved_ss = (signal_state_t *)t->signals;
        int mask_ret = signal_task_set_temporary_mask(
            t, arch_user_sigset_to_kernel(&user_mask), &saved_blocked);
        if (mask_ret < 0) {
            epoll_put_ref(ep_vf);
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

    /*
     * Scratch arrays are sized to the registered-item count rather than
     * EPOLL_MAX_FDS: a hot epoll_wait() would otherwise zero ~96 KiB per call
     * (kcalloc plus an explicit memset of both 1024-entry arrays).  Both arrays
     * are fully written for [0, interest_count) below, so zeroing is unneeded.
     * epoll_ctl() during the wait bumps change_seq (forcing a re-snapshot) and
     * may grow ep->count; the overflow path falls back to the hard maximum.
     */
    size_t scratch_cap = 0;
    bool force_max = false;
    readiness_interest_t *interests = NULL;
    epoll_wait_item_t *snapshots = NULL;

    int total_ready = 0;
    for (;;) {
        size_t need;
        if (force_max) {
            need = EPOLL_MAX_FDS;
        } else {
            uint64_t cap_flags = spin_lock_irqsave(&ep->lock);
            need = (size_t)ep->count;
            spin_unlock_irqrestore(&ep->lock, cap_flags);
            if (need == 0)
                need = 1;
        }
        if (need > scratch_cap) {
            readiness_interest_t *new_interests =
                kmalloc(need * sizeof(*new_interests));
            epoll_wait_item_t *new_snapshots =
                kmalloc(need * sizeof(*new_snapshots));
            if (!new_interests || !new_snapshots) {
                kfree(new_interests);
                kfree(new_snapshots);
                kfree(interests);
                kfree(snapshots);
                if (sigmask && saved_ss)
                    signal_task_restore_mask(t, saved_blocked);
                epoll_put_ref(ep_vf);
                return -ENOMEM;
            }
            kfree(interests);
            kfree(snapshots);
            interests = new_interests;
            snapshots = new_snapshots;
            scratch_cap = need;
        }
        size_t interest_count = 0;
        bool overflow = false;
        uint64_t lock_flags = spin_lock_irqsave(&ep->lock);
        epoll_change_probe_t probe = {
            .ep = ep,
            .sequence = ep->change_seq,
            .snapshots = snapshots,
            .count = 0,
        };
        for (int i = 0; i < EPOLL_MAX_FDS; i++) {
            if (!ep->items[i].registered)
                continue;
            if (interest_count >= scratch_cap) {
                overflow = true;
                break;
            }
            size_t current = interest_count;
            snapshots[current] = (epoll_wait_item_t){
                .index = i,
                .fd = ep->items[i].fd,
                .identity = ep->items[i].identity,
                .ev = ep->items[i].ev,
                .state = ep->items[i].state,
                .target = ep->targets[i],
                .pruned = 0,
            };
            if (snapshots[current].target)
                vfile_get(snapshots[current].target);
            interests[current] = (readiness_interest_t){
                .fd = -1,
                .vfile = snapshots[current].target,
                .events = epoll_events_to_poll(ep->items[i].ev.events),
                .cookie = current,
                .flags = READINESS_F_GLOBAL_FD,
                .expected_identity = ep->items[i].identity,
                .state = &snapshots[current].state,
#ifdef CONFIG_EVENTPOLL_STATE_RUST
                .filter = epoll_filter_ready_rust,
                .filter_arg = &snapshots[current].state,
#endif
            };
            interest_count++;
        }
        spin_unlock_irqrestore(&ep->lock, lock_flags);
        if (overflow) {
            for (size_t j = 0; j < interest_count; j++)
                if (snapshots[j].target)
                    vfs_put_file(snapshots[j].target);
            force_max = true;
            continue;
        }
        probe.count = interest_count;
        readiness_extra_t control[2] = {
            {
                .source = { &ep->control_waiters, 0, 0 },
                .ready = epoll_change_pending,
                .arg = &probe,
            },
            {
                .source = { fdtable_global_close_readiness_queue(), 0, 0 },
                .ready = epoll_change_pending,
                .arg = &probe,
            },
        };
        bool stale_target = false;
        for (size_t j = 0; j < interest_count; j++) {
            if (!snapshots[j].target ||
                !fdtable_file_has_open_slot(snapshots[j].target)) {
                interests[j].revents = POLLNVAL;
                stale_target = true;
            }
        }
        int wait_result = stale_target ? 1 : readiness_wait_once(
            interests, interest_count, control, 2, (size_t)maxevents,
            deadline, has_timeout);

        bool pruned = false;
        lock_flags = spin_lock_irqsave(&ep->lock);
        if (ep->change_seq != probe.sequence) {
            spin_unlock_irqrestore(&ep->lock, lock_flags);
            for (size_t j = 0; j < interest_count; j++)
                if (snapshots[j].target)
                    vfs_put_file(snapshots[j].target);
            continue;
        }
        for (size_t j = 0; j < interest_count; j++) {
            epoll_wait_item_t *snapshot = &snapshots[j];
#ifndef CONFIG_EVENTPOLL_STATE_RUST
            epoll_item_t *item = &ep->items[snapshot->index];
#endif
#ifdef CONFIG_EVENTPOLL_STATE_RUST
            bool removed = a20_eventpoll_commit_rust(
                ep->items, &ep->count, (size_t)snapshot->index,
                snapshot->fd, snapshot->identity, &snapshot->state,
                (interests[j].revents & POLLNVAL) != 0);
            pruned |= removed;
            if (removed) {
                ep->targets[snapshot->index] = NULL;
                __atomic_sub_fetch(&g_epoll_watch_count, 1,
                                   __ATOMIC_RELEASE);
                snapshot->pruned = 1;
            }
#else
            if (!item->registered || item->fd != snapshot->fd ||
                item->identity != snapshot->identity)
                continue;
            item->state = snapshot->state;
            if (interests[j].revents & POLLNVAL) {
                item->registered = 0;
                item->fd = -1;
                item->identity = 0;
                ep->count--;
                __atomic_sub_fetch(&g_epoll_watch_count, 1,
                                   __ATOMIC_RELEASE);
                pruned = true;
                ep->targets[snapshot->index] = NULL;
                snapshot->pruned = 1;
            }
#endif
        }
        if (pruned)
            epoll_change_locked(ep);
        spin_unlock_irqrestore(&ep->lock, lock_flags);
        for (size_t j = 0; j < interest_count; j++) {
            if (!snapshots[j].target)
                continue;
            vfs_put_file(snapshots[j].target); /* snapshot reference */
            if (snapshots[j].pruned)
                vfs_put_file(snapshots[j].target); /* registration reference */
        }
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
    epoll_put_ref(ep_vf);
    return total_ready;
}

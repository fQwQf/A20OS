#include "fs/inotify.h"
#include "fs/fanotify.h"

#include "core/consts.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/sync.h"
#include "fs/anonfd.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

#define INOTIFY_QUEUE_CAP 64

typedef struct inotify_watch {
    struct inotify_watch *next;
    struct inotify_instance *instance;
    struct vnode *vnode;   /* referenced */
    int   wd;
    uint32_t mask;
} inotify_watch_t;

typedef struct inotify_event_q {
    int   wd;
    uint32_t mask;
    uint32_t cookie;
    uint64_t ino;                       /* fanotify FID (target inode) */
    char  name[MAX_NAME_LEN + 1];
} inotify_event_q_t;

typedef struct inotify_instance {
    struct inotify_instance *next;   /* global list, g_inotify_lock */
    spinlock_t lock;
    wait_queue_t readers;
    inotify_watch_t *watches;
    inotify_event_q_t queue[INOTIFY_QUEUE_CAP];
    unsigned head;
    unsigned count;
    int   next_wd;
    uint32_t next_cookie;
    int   nonblock;
    int   overflow_pending;
    int   fanotify;          /* fanotify instance mode */
    int   fanotify_flags;    /* init flags (report FID/name) */
} inotify_instance_t;

static inotify_instance_t *g_instances;
static spinlock_t g_inotify_lock = SPINLOCK_INIT;

static int inotify_ops_read(vfile_t *vf, char *buf, size_t count);

static inotify_instance_t *inotify_from_gfd(int gfd)
{
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf)
        return NULL;
    inotify_instance_t *inst = NULL;
    if (vf->ops && vf->ops->read == inotify_ops_read)
        inst = vf->priv;
    vfs_put_file_ref(gfd, vf);
    return inst;
}

static void inotify_queue_event(inotify_instance_t *inst, int wd,
                                uint32_t mask, uint32_t cookie,
                                uint64_t ino, const char *name)
{
    if (inst->count >= INOTIFY_QUEUE_CAP) {
        inst->overflow_pending = 1;
        return;
    }
    inotify_event_q_t *ev = &inst->queue[(inst->head + inst->count) %
                                         INOTIFY_QUEUE_CAP];
    ev->wd = wd;
    ev->mask = mask;
    ev->cookie = cookie;
    ev->ino = ino;
    ev->name[0] = '\0';
    if (name)
        strncpy(ev->name, name, MAX_NAME_LEN);
    inst->count++;
}

static void inotify_notify_locked(inotify_instance_t *inst,
                                  struct vnode *vn, const char *name,
                                  uint32_t mask)
{
    int hit = 0;
    for (inotify_watch_t *w = inst->watches; w; w = w->next) {
        if (w->vnode != vn)
            continue;
        uint32_t emask = mask & w->mask;
        if (!emask)
            continue;
        uint32_t evmask = emask;
        if (vn->type == VFS_FT_DIR)
            evmask |= IN_ISDIR;
        inotify_queue_event(inst, w->wd, evmask, 0, vn->ino, name);
        hit = 1;
        if (w->mask & IN_ONESHOT)
            w->mask = 0;
    }
    if (hit)
        wait_queue_wake_all(&inst->readers, 0, PROC_WAKE_EVENT);
}

void inotify_vnode_event(struct vnode *vn, const char *name, uint32_t mask)
{
    if (!vn)
        return;

    spin_lock(&g_inotify_lock);
    for (inotify_instance_t *inst = g_instances; inst; inst = inst->next) {
        if (!inst->watches)
            continue;
        spin_lock(&inst->lock);
        inotify_notify_locked(inst, vn, name, mask);
        spin_unlock(&inst->lock);
    }
    spin_unlock(&g_inotify_lock);
}

/* ---- fanotify event format ---- */

struct fanotify_event_info_header {
    uint8_t info_type;
    uint8_t pad;
    uint16_t len;
};

struct fanotify_event_info_fid {
    struct fanotify_event_info_header hdr;
    int32_t fsid[2];
    uint32_t handle_bytes;
    int32_t handle_type;
    uint64_t ino;
} __attribute__((packed));

_Static_assert(sizeof(struct fanotify_event_info_fid) == 28,
               "fanotify FID info must match the Linux userspace ABI");

struct fanotify_event_metadata {
    uint32_t event_len;
    uint8_t  vers;
    uint8_t  reserved;
    uint16_t metadata_len;
    uint64_t mask;
    int32_t  fd;
    int32_t  pid;
};

/* Dequeue the next event into the on-stack `ev`/`namebuf` buffers; returns
 * 1 if an event was produced, 0 on empty, or a negative errno. */
static int inotify_pop_locked(inotify_instance_t *inst,
                              struct inotify_event *ev, char *namebuf,
                              size_t *nlen, uint64_t *ino)
{
    if (inst->overflow_pending) {
        inst->overflow_pending = 0;
        ev->wd = -1;
        ev->mask = IN_Q_OVERFLOW;
        ev->cookie = 0;
        ev->len = 0;
        *nlen = 0;
        *ino = 0;
        return 1;
    }
    if (inst->count == 0)
        return 0;
    inotify_event_q_t *q = &inst->queue[inst->head];
    ev->wd = q->wd;
    ev->mask = q->mask;
    ev->cookie = q->cookie;
    *ino = q->ino;
    *nlen = strlen(q->name);
    ev->len = (uint32_t)(*nlen + 1);
    memcpy(namebuf, q->name, *nlen + 1);
    inst->head = (inst->head + 1) % INOTIFY_QUEUE_CAP;
    inst->count--;
    return 1;
}

static int inotify_ops_read(vfile_t *vf, char *buf, size_t count)
{
    inotify_instance_t *inst = vf ? vf->priv : NULL;
    if (!inst)
        return -EBADF;
    if (count < (inst->fanotify ? sizeof(struct fanotify_event_metadata)
                                : sizeof(struct inotify_event)))
        return -EINVAL;

    /* Wait for the first event (without holding the instance lock). */
    spin_lock(&inst->lock);
    while (inst->count == 0 && !inst->overflow_pending) {
        if (inst->nonblock) {
            spin_unlock(&inst->lock);
            return -EAGAIN;
        }
        spin_unlock(&inst->lock);
        proc_wait_token_t token =
            proc_park_prepare(PROC_WAIT_INTERRUPTIBLE, 0);
        if (!token.task)
            return -EAGAIN;
        wait_queue_entry_t entry = {0};
        spin_lock(&inst->lock);
        if (inst->count != 0 || inst->overflow_pending) {
            spin_unlock(&inst->lock);
            (void)proc_park_cancel(token);
            proc_park_finish(token);
            continue;
        }
        bool linked = wait_queue_link(&inst->readers, &entry, token, 0);
        spin_unlock(&inst->lock);
        proc_wake_reason_t reason;
        if (linked)
            reason = proc_park_commit(token);
        else {
            (void)proc_park_cancel(token);
            reason = PROC_WAKE_CANCEL;
        }
        wait_queue_unlink(&inst->readers, &entry);
        proc_park_finish(token);
        if (proc_wake_reason_is_task_interrupt(reason))
            return -ERESTARTSYS;
        spin_lock(&inst->lock);
    }

    size_t written = 0;
    for (;;) {
        struct inotify_event ev;
        char namebuf[MAX_NAME_LEN + 1];
        size_t nlen = 0;
        uint64_t ino = 0;

        spin_lock(&inst->lock);
        int pop = inotify_pop_locked(inst, &ev, namebuf, &nlen, &ino);
        spin_unlock(&inst->lock);
        if (pop <= 0)
            break;

        if (inst->fanotify) {
            struct fanotify_event_metadata fm;
            struct fanotify_event_info_fid fid;
            size_t info_len = sizeof(fid);
            size_t ev_size = sizeof(fm) + info_len;
            if (ev.len > 0)
                ev_size += (size_t)ev.len;
            if (written + ev_size > count)
                break;
            fm.event_len = (uint32_t)ev_size;
            fm.vers = FANOTIFY_METADATA_VERSION;
            fm.reserved = 0;
            fm.metadata_len = (uint16_t)sizeof(fm);
            fm.mask = ev.mask;
            fm.fd = FAN_NOFD;
            fm.pid = proc_current() ? proc_current()->pid : 0;

            memset(&fid, 0, sizeof(fid));
            fid.hdr.info_type = FAN_EVENT_INFO_TYPE_FID;
            fid.hdr.len = (uint16_t)sizeof(fid);
            fid.handle_bytes = sizeof(fid.ino);
            fid.ino = ino;

            char *p = buf + written;
            if (copy_to_user(p, &fm, sizeof(fm)) < 0)
                return -EFAULT;
            if (copy_to_user(p + sizeof(fm), &fid, sizeof(fid)) < 0)
                return -EFAULT;
            if (ev.len > 0 &&
                copy_to_user(p + sizeof(fm) + sizeof(fid), namebuf,
                             (size_t)ev.len) < 0)
                return -EFAULT;
            written += ev_size;
        } else {
            size_t ev_size = sizeof(ev) + ev.len;
            if (written + ev_size > count)
                break;
            if (ev.len > 0 && copy_to_user(buf + written + sizeof(ev), namebuf,
                                           (size_t)ev.len) < 0)
                return -EFAULT;
            if (copy_to_user(buf + written, &ev, sizeof(ev)) < 0)
                return -EFAULT;
            written += ev_size;
        }
    }
    return (int)written;
}

static int inotify_ops_poll(vfile_t *vf, short events)
{
    inotify_instance_t *inst = vf ? vf->priv : NULL;
    if (!inst)
        return POLLNVAL;
    short revents = 0;
    spin_lock(&inst->lock);
    if ((events & POLLIN) && (inst->count != 0 || inst->overflow_pending))
        revents |= POLLIN;
    spin_unlock(&inst->lock);
    return revents;
}

static int inotify_ops_close(vfile_t *vf)
{
    inotify_instance_t *inst = vf ? vf->priv : NULL;
    if (!inst)
        return 0;

    spin_lock(&g_inotify_lock);
    inotify_instance_t **pp = &g_instances;
    while (*pp && *pp != inst)
        pp = &(*pp)->next;
    if (*pp)
        *pp = inst->next;
    spin_unlock(&g_inotify_lock);

    inotify_watch_t *w = inst->watches;
    while (w) {
        inotify_watch_t *nw = w->next;
        if (w->vnode)
            vnode_put(w->vnode);
        kfree(w);
        w = nw;
    }
    kfree(inst);
    vf->priv = NULL;
    return 0;
}

static vfile_ops_t g_inotify_ops = {
    .read = inotify_ops_read,
    .poll = inotify_ops_poll,
    .close = inotify_ops_close,
};

static int inotify_create_common(int flags, int fanotify, int fanotify_flags)
{
    int allowed = O_CLOEXEC | O_NONBLOCK;
    if (flags & ~allowed)
        return -EINVAL;

    inotify_instance_t *inst = kcalloc(1, sizeof(*inst));
    vfile_t *vf = vfile_alloc();
    if (!inst || !vf) {
        if (inst) kfree(inst);
        if (vf) vfile_free(vf);
        return -ENOMEM;
    }
    spin_init(&inst->lock);
    wait_queue_init(&inst->readers);
    inst->next_wd = 1;
    inst->nonblock = (flags & O_NONBLOCK) != 0;
    inst->fanotify = fanotify;
    inst->fanotify_flags = fanotify_flags;
    vf->priv = inst;
    vf->ops = &g_inotify_ops;
    vfile_ref_init(vf, 1);

    spin_lock(&g_inotify_lock);
    inst->next = g_instances;
    g_instances = inst;
    spin_unlock(&g_inotify_lock);

    return anonfd_install_vfile(vf, flags);
}

int inotify_create_file(int flags)
{
    return inotify_create_common(flags, 0, 0);
}

int fanotify_create_file(int flags, int event_f_flags)
{
    (void)event_f_flags;
    /* A20OS implements the FAN_CLASS_NOTIF subset only, which never opens
     * files for content/pre-content access, so the event file flags are
     * accepted but not applied to any per-event fd. */
    return inotify_create_common(flags, 1, flags);
}

static int fanotify_add_watch(inotify_instance_t *inst, struct vnode *vn,
                              uint64_t mask, int onlydir)
{
    int create_only = (mask & IN_MASK_CREATE) != 0;
    int add_events = (mask & IN_MASK_ADD) != 0;
    uint32_t wmask = (uint32_t)(mask & (IN_ALL_EVENTS | IN_ONESHOT));
    if (wmask == 0)
        return -EINVAL;
    if (onlydir)
        wmask |= IN_ONLYDIR;

    spin_lock(&inst->lock);
    for (inotify_watch_t *w = inst->watches; w; w = w->next) {
        if (w->vnode == vn) {
            if (create_only) {
                spin_unlock(&inst->lock);
                vnode_put(vn);
                return -EEXIST;
            }
            w->mask = add_events ? (w->mask | wmask) : wmask;
            int wd = w->wd;
            spin_unlock(&inst->lock);
            vnode_put(vn);
            return wd;
        }
    }

    inotify_watch_t *w = kcalloc(1, sizeof(*w));
    if (!w) {
        spin_unlock(&inst->lock);
        vnode_put(vn);
        return -ENOMEM;
    }
    w->wd = inst->next_wd++;
    w->mask = wmask;
    w->vnode = vn;
    w->instance = inst;
    w->next = inst->watches;
    inst->watches = w;
    int wd = w->wd;
    spin_unlock(&inst->lock);
    return wd;
}

int fanotify_mark(int gfd, unsigned flags, uint64_t mask, int dfd,
                  const char *path)
{
    (void)dfd;
    const unsigned allowed = FAN_MARK_ADD | FAN_MARK_REMOVE |
                             FAN_MARK_DONT_FOLLOW | FAN_MARK_ONLYDIR;
    const uint64_t allowed_mask =
        IN_ALL_EVENTS | FAN_EVENT_ON_CHILD | FAN_ONDIR;
    if (!path)
        return -EFAULT;
    if (flags & ~allowed)
        return -EINVAL;
    if (!!(flags & FAN_MARK_ADD) == !!(flags & FAN_MARK_REMOVE))
        return -EINVAL;
    if (!mask || (mask & ~allowed_mask))
        return -EINVAL;

    inotify_instance_t *inst = inotify_from_gfd(gfd);
    if (!inst)
        return -EBADF;
    if (!inst->fanotify) {
        return -EINVAL;
    }

    struct vnode *vn = (flags & FAN_MARK_DONT_FOLLOW)
                           ? vfs_resolve_no_follow_final(path)
                           : vfs_resolve_no_follow(path);
    if (!vn)
        return -ENOENT;

    if (flags & FAN_MARK_ONLYDIR) {
        if (vn->type != VFS_FT_DIR) {
            vnode_put(vn);
            return -ENOTDIR;
        }
    }

    if (flags & FAN_MARK_REMOVE) {
        spin_lock(&inst->lock);
        inotify_watch_t **pp = &inst->watches;
        int removed = 0;
        while (*pp) {
            if ((*pp)->vnode == vn) {
                inotify_watch_t *w = *pp;
                *pp = w->next;
                if (w->vnode)
                    vnode_put(w->vnode);
                kfree(w);
                removed = 1;
            } else {
                pp = &(*pp)->next;
            }
        }
        spin_unlock(&inst->lock);
        vnode_put(vn);
        return removed ? 0 : -ENOENT;
    }

    /* fanotify_mark returns 0 on success (unlike inotify_add_watch's wd). */
    int wd = fanotify_add_watch(inst, vn, mask, (flags & FAN_MARK_ONLYDIR) != 0);
    if (wd < 0)
        return wd;
    return 0;
}

int inotify_add_watch(int gfd, const char *path, uint32_t mask)
{
    if (!path)
        return -EINVAL;

    inotify_instance_t *inst = inotify_from_gfd(gfd);
    if (!inst)
        return -EBADF;
    if (inst->fanotify)
        return -EINVAL;

    int create_only = (mask & IN_MASK_CREATE) != 0;
    int add_events = (mask & IN_MASK_ADD) != 0;
    uint32_t wmask = mask & (IN_ALL_EVENTS | IN_ONESHOT);
    if (wmask == 0)
        return -EINVAL;

    struct vnode *vn = vfs_resolve_no_follow_final(path);
    if (!vn)
        return -ENOENT;

    spin_lock(&inst->lock);
    for (inotify_watch_t *w = inst->watches; w; w = w->next) {
        if (w->vnode == vn) {
            if (create_only) {
                spin_unlock(&inst->lock);
                vnode_put(vn);
                return -EEXIST;
            }
            w->mask = add_events ? (w->mask | wmask) : wmask;
            int wd = w->wd;
            spin_unlock(&inst->lock);
            vnode_put(vn);
            return wd;
        }
    }

    inotify_watch_t *w = kcalloc(1, sizeof(*w));
    if (!w) {
        spin_unlock(&inst->lock);
        vnode_put(vn);
        return -ENOMEM;
    }
    w->wd = inst->next_wd++;
    w->mask = wmask;
    w->vnode = vn;
    w->instance = inst;
    w->next = inst->watches;
    inst->watches = w;
    int wd = w->wd;
    spin_unlock(&inst->lock);
    return wd;
}

int inotify_rm_watch(int gfd, int wd)
{
    inotify_instance_t *inst = inotify_from_gfd(gfd);
    if (!inst)
        return -EBADF;

    spin_lock(&inst->lock);
    inotify_watch_t **pp = &inst->watches;
    inotify_watch_t *found = NULL;
    while (*pp) {
        if ((*pp)->wd == wd) {
            found = *pp;
            *pp = (*pp)->next;
            break;
        }
        pp = &(*pp)->next;
    }
    spin_unlock(&inst->lock);
    if (!found)
        return -EINVAL;
    if (found->vnode)
        vnode_put(found->vnode);
    kfree(found);
    return 0;
}

#include "fs/inotify.h"

#include "core/consts.h"
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
                                const char *name)
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
        inotify_queue_event(inst, w->wd, evmask, 0, name);
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

static int inotify_ops_read(vfile_t *vf, char *buf, size_t count)
{
    inotify_instance_t *inst = vf ? vf->priv : NULL;
    if (!inst)
        return -EBADF;
    if (count < sizeof(struct inotify_event))
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

    /* Pop events one at a time; copy to user only after releasing the lock. */
    size_t written = 0;
    for (;;) {
        struct inotify_event ev;
        char namebuf[MAX_NAME_LEN + 1];
        size_t nlen = 0;

        spin_lock(&inst->lock);
        if (inst->overflow_pending) {
            inst->overflow_pending = 0;
            ev.wd = -1;
            ev.mask = IN_Q_OVERFLOW;
            ev.cookie = 0;
            ev.len = 0;
        } else if (inst->count > 0) {
            inotify_event_q_t *q = &inst->queue[inst->head];
            ev.wd = q->wd;
            ev.mask = q->mask;
            ev.cookie = q->cookie;
            nlen = strlen(q->name);
            ev.len = (uint32_t)(nlen + 1);
            memcpy(namebuf, q->name, nlen + 1);
            inst->head = (inst->head + 1) % INOTIFY_QUEUE_CAP;
            inst->count--;
        } else {
            spin_unlock(&inst->lock);
            break;
        }
        spin_unlock(&inst->lock);

        size_t ev_size = sizeof(ev) + ev.len;
        if (written + ev_size > count)
            break;
        if (ev.len > 0 && copy_to_user(buf + written + sizeof(ev), namebuf,
                                       ev.len) < 0)
            return -EFAULT;
        if (copy_to_user(buf + written, &ev, sizeof(ev)) < 0)
            return -EFAULT;
        written += ev_size;
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

int inotify_create_file(int flags)
{
    if (flags & ~(O_CLOEXEC | O_NONBLOCK))
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
    vf->priv = inst;
    vf->ops = &g_inotify_ops;
    vfile_ref_init(vf, 1);

    spin_lock(&g_inotify_lock);
    inst->next = g_instances;
    g_instances = inst;
    spin_unlock(&g_inotify_lock);

    return anonfd_install_vfile(vf, flags);
}

int inotify_add_watch(int gfd, const char *path, uint32_t mask)
{
    if (!path)
        return -EINVAL;

    inotify_instance_t *inst = inotify_from_gfd(gfd);
    if (!inst)
        return -EBADF;

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

#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

typedef struct {
    uint32_t events;
    uint64_t data;
} epoll_event_t;

#define EPOLL_MAX 64
typedef struct {
    uint64_t used;
    int fd[EPOLL_MAX];
    epoll_event_t ev[EPOLL_MAX];
} epoll_t;

static int epoll_close(vfile_t *vf)
{
    if (vf && vf->priv) kfree(vf->priv);
    return 0;
}

static vfile_ops_t g_epoll_ops = {
    .close = epoll_close,
};

static epoll_t *epoll_get_file(int epfd)
{
    int64_t gfd = fdtable_get_current(epfd);
    if (gfd < 0) return NULL;
    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return NULL;
    if (!vf->priv) {
        epoll_t *ep = kmalloc(sizeof(*ep));
        if (!ep) {
            vfs_put_file_ref((int)gfd, vf);
            return NULL;
        }
        memset(ep, 0, sizeof(*ep));
        vf->priv = ep;
        vf->ops = &g_epoll_ops;
    }
    epoll_t *ep = (epoll_t *)vf->priv;
    vfs_put_file_ref((int)gfd, vf);
    return ep;
}

int64_t sys_epoll_ctl(int epfd, int op, int fd, void *event)
{
    epoll_t *ep = epoll_get_file(epfd);
    if (!ep) return -EBADF;
    if (fdtable_get_current(fd) < 0) return -EBADF;
    epoll_event_t ev;
    memset(&ev, 0, sizeof(ev));
    if (event && copy_from_user(&ev, event, sizeof(ev)) < 0) return -EFAULT;
    int idx = -1;
    for (int i = 0; i < EPOLL_MAX; i++)
        if (ep->used & (1ULL << i) && ep->fd[i] == fd) idx = i;
    if (op == 1) {
        if (idx >= 0) return -EEXIST;
        for (int i = 0; i < EPOLL_MAX; i++) {
            if (!(ep->used & (1ULL << i))) {
                ep->used |= (1ULL << i);
                ep->fd[i] = fd;
                ep->ev[i] = ev;
                return 0;
            }
        }
        return -ENOSPC;
    }
    if (op == 2) {
        if (idx < 0) return -ENOENT;
        ep->ev[idx] = ev;
        return 0;
    }
    if (op == 3) {
        if (idx < 0) return -ENOENT;
        ep->used &= ~(1ULL << idx);
        return 0;
    }
    return -EINVAL;
}

int64_t sys_epoll_pwait(int epfd, void *events, int maxevents, int timeout, const void *sigmask, size_t sigsetsize)
{
    (void)timeout; (void)sigmask; (void)sigsetsize;
    epoll_t *ep = epoll_get_file(epfd);
    if (!ep) return -EBADF;
    if (!events || maxevents <= 0) return -EINVAL;
    int n = 0;
    for (int i = 0; i < EPOLL_MAX && n < maxevents; i++) {
        if (!(ep->used & (1ULL << i))) continue;
        if (fdtable_get_current(ep->fd[i]) < 0) continue;
        if (copy_to_user((char *)events + (size_t)n * sizeof(epoll_event_t),
                         &ep->ev[i], sizeof(epoll_event_t)) < 0)
            return -EFAULT;
        n++;
    }
    if (n == 0 && timeout != 0) proc_yield();
    return n;
}

int64_t sys_epoll_wait(int epfd, void *events, int maxevents, int timeout)
{
    return sys_epoll_pwait(epfd, events, maxevents, timeout, NULL, 0);
}

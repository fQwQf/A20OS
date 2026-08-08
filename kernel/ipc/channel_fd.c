/*
 * A20OS — channel endpoints as Linux file descriptors.
 *
 * This is the ABI-agnostic bridge that lets a Linux ABI process use the
 * Native ABI's channel IPC through the standard fd surface: read()/write()
 * map onto one channel message each (SOCK_SEQPACKET-like semantics),
 * poll() reports readability/writability, and close() releases the endpoint.
 *
 * The wrapped objects are the same a20_channel_ep_t instances the Native ABI
 * references through its handle table, so Linux and Native programs share
 * one IPC mechanism (the service layer, the supervisor, sockets, ...).
 */
#include "core/types.h"
#include "core/string.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/poll.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/anonfd.h"
#include "ipc/ipc.h"
#include "ipc/channel_fd.h"

static vfile_ops_t a20_ch_fd_ops;

static int a20_ch_errno(int64_t r)
{
    switch (r) {
    case -A20_ERR_WOULD_BLOCK:      return -EAGAIN;
    case -A20_ERR_INTERRUPTED:      return -EINTR;
    case -A20_ERR_CANCELED:         return -EPIPE;   /* peer endpoint closed */
    case -A20_ERR_BAD_HANDLE:       return -EBADF;
    case -A20_ERR_NO_MEMORY:        return -ENOMEM;
    case -A20_ERR_ACCESS:           return -EACCES;
    case -A20_ERR_FAULT:            return -EFAULT;
    case -A20_ERR_INVALID_ARGUMENT: return -EINVAL;
    case -A20_ERR_NO_SPACE:         return -EMSGSIZE;
    case -A20_ERR_NOT_SUPPORTED:    return -ENOSYS;
    case -A20_ERR_TIMED_OUT:        return -ETIMEDOUT;
    default:                        return -EIO;
    }
}

static int a20_ch_fd_read(vfile_t *vf, char *buf, size_t count)
{
    a20_channel_ep_t *ep = vf ? (a20_channel_ep_t *)vf->priv : NULL;
    if (!ep)
        return -EBADF;
    uint32_t nonblock = (vf->flags & O_NONBLOCK) ? A20_MSG_NONBLOCK : 0;
    if (count > A20_CH_MAX_DATA)
        count = A20_CH_MAX_DATA;

    uint32_t dlen = 0, nh = 0;
    int64_t r = a20_channel_recv_begin(ep, nonblock, &dlen, &nh);
    if (r == -A20_ERR_CANCELED)
        return 0;   /* peer closed: EOF, like a pipe/socket read */
    if (r < 0)
        return a20_ch_errno(r);

    uint32_t want = (uint32_t)count;
    a20_ch_handle_info_t hinfo[A20_CH_MAX_HANDLES];
    uint32_t hc = A20_CH_MAX_HANDLES;
    r = a20_channel_recv_finish(ep, buf, &want, hinfo, &hc);
    if (r < 0)
        return a20_ch_errno(r);

    /* The fd surface has no capability API: drop any message-carried
     * handle references (released, not delivered). */
    for (uint32_t i = 0; i < hc; i++)
        a20_object_release(hinfo[i].object, hinfo[i].type);
    return (int)want;
}

static int a20_ch_fd_write(vfile_t *vf, const char *buf, size_t count)
{
    a20_channel_ep_t *ep = vf ? (a20_channel_ep_t *)vf->priv : NULL;
    if (!ep)
        return -EBADF;
    if (count > A20_CH_MAX_DATA)
        return -EMSGSIZE;
    uint32_t nonblock = (vf->flags & O_NONBLOCK) ? A20_MSG_NONBLOCK : 0;
    int64_t r = a20_channel_send(ep, buf, (uint32_t)count, NULL, 0, NULL,
                                 nonblock);
    if (r < 0)
        return a20_ch_errno(r);
    return (int)count;
}

static int a20_ch_fd_poll(vfile_t *vf, short events)
{
    a20_channel_ep_t *ep = vf ? (a20_channel_ep_t *)vf->priv : NULL;
    if (!ep)
        return -EBADF;
    short rev = 0;
    int closed = a20_channel_peer_closed(ep);
    if (closed)
        rev |= POLLHUP;
    if ((events & POLLIN) && (a20_channel_readable(ep) || closed))
        rev |= POLLIN;
    if ((events & POLLOUT) && a20_channel_writable(ep))
        rev |= POLLOUT;
    return rev;
}

static int a20_ch_fd_close(vfile_t *vf)
{
    a20_channel_ep_t *ep = vf ? (a20_channel_ep_t *)vf->priv : NULL;
    if (ep) {
        a20_channel_ep_release(ep);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t a20_ch_fd_ops = {
    .read  = a20_ch_fd_read,
    .write = a20_ch_fd_write,
    .poll  = a20_ch_fd_poll,
    .close = a20_ch_fd_close,
};

/*
 * Wrap @ep (which must carry a reference owned by the new fd) as a file
 * descriptor in the current task's fd table.  On failure the reference is
 * released.
 */
int a20_channel_fd_install(a20_channel_ep_t *ep, int flags)
{
    if (!ep)
        return -EBADF;
    vfile_t *vf = vfile_alloc();
    if (!vf) {
        a20_channel_ep_release(ep);
        return -ENOMEM;
    }
    refcount_set(&vf->ref_count, 1);
    vf->flags = flags;
    vf->ops = &a20_ch_fd_ops;
    vf->priv = ep;
    return anonfd_install_vfile(vf, flags);
}

#include "fs/splice.h"

#include "core/consts.h"
#include "core/string.h"
#include "fs/fdtable.h"
#include "fs/file.h"
#include "fs/pipe.h"
#include "fs/vfs.h"
#include "proc/proc.h"
#include "sys/usercopy.h"

#define SPLICE_CHUNK_SIZE 65536

/*
 * Core splice/tee/vmsplice engine.
 *
 * A20OS pipes are plain ring buffers, so transfers are copy-based.  This is
 * semantically valid: Linux itself falls back to buffered copies when the
 * pipe buffers are not page-aligned.  The ABI wrapper validates the Linux
 * flags and resolves user pointers; everything below operates on vfile
 * objects and kernel-side offsets only.
 */

/* Set or clear O_NONBLOCK on a pipe vfile for the duration of a transfer so
 * SPLICE_F_NONBLOCK controls the pipe side independently of the fd flags.
 * Returns the previous O_NONBLOCK state so the caller can restore it. */
static int splice_pipe_set_nonblock(vfile_t *vf, int nonblock)
{
    int saved = vf->flags & O_NONBLOCK;
    if (nonblock)
        vf->flags |= O_NONBLOCK;
    else
        vf->flags &= ~O_NONBLOCK;
    return saved;
}

static void splice_pipe_restore_nonblock(vfile_t *vf, int saved)
{
    if (saved)
        vf->flags |= O_NONBLOCK;
    else
        vf->flags &= ~O_NONBLOCK;
}

/* Position a regular file at offset, returning the saved offset.  When off
 * is NULL the current position is used unchanged (-1 sentinel = no seek). */
static long splice_seek_to(vfile_t *vf, const long *off)
{
    if (!vf->vnode || !vf->ops || !vf->ops->lseek)
        return -ESPIPE;
    long saved = vf->ops->lseek(vf, 0, SEEK_CUR);
    if (saved < 0)
        return saved;
    if (off) {
        if (vf->ops->lseek(vf, *off, SEEK_SET) < 0)
            return -EINVAL;
    }
    return saved;
}

static int splice_seek_restore(vfile_t *vf, long saved)
{
    if (!vf->ops || !vf->ops->lseek)
        return -EINVAL;
    return vf->ops->lseek(vf, saved, SEEK_SET);
}

static int splice_write_to(vfile_t *out_vf, const char *buf, size_t len,
                           int nonblock)
{
    splice_pipe_set_nonblock(out_vf, nonblock);
    return vfs_write_file(out_vf, buf, len);
}

static int splice_read_from(vfile_t *in_vf, char *buf, size_t len,
                            int nonblock)
{
    if (pipe_vfile_is(in_vf))
        splice_pipe_set_nonblock(in_vf, nonblock);
    return vfs_read_file(in_vf, buf, len);
}

/* Transfer up to len bytes between two endpoints.  at least one of them must
 * be a pipe; the caller's wrapper enforces that.  Offsets are applied to the
 * file endpoints only; when an offset pointer is NULL the file's current
 * position is used and advanced (Linux splice semantics).  The offset values
 * are updated on success. */
int splice_do(int fd_in, long *off_in, int fd_out, long *off_out, size_t len,
              int nonblock)
{
    if (len == 0)
        return 0;

    vfile_t *in_vf = vfs_get_file_ref(fd_in);
    if (!in_vf)
        return -EBADF;
    vfile_t *out_vf = vfs_get_file_ref(fd_out);
    if (!out_vf) {
        vfs_put_file_ref(fd_in, in_vf);
        return -EBADF;
    }

    char *buf = proc_scratch_buffer(SPLICE_CHUNK_SIZE);
    if (!buf) {
        vfs_put_file_ref(fd_in, in_vf);
        vfs_put_file_ref(fd_out, out_vf);
        return -ENOMEM;
    }

    int in_pipe = pipe_vfile_is(in_vf);
    int out_pipe = pipe_vfile_is(out_vf);
    int in_nb = in_pipe ? (in_vf->flags & O_NONBLOCK) : 0;
    int out_nb = out_pipe ? (out_vf->flags & O_NONBLOCK) : 0;

    long in_saved = -1, out_saved = -1;
    if (!in_pipe) {
        in_saved = splice_seek_to(in_vf, off_in);
        if (in_saved < 0) {
            vfs_put_file_ref(fd_in, in_vf);
            vfs_put_file_ref(fd_out, out_vf);
            return (int)in_saved;
        }
    }
    if (!out_pipe) {
        out_saved = splice_seek_to(out_vf, off_out);
        if (out_saved < 0) {
            if (in_saved >= 0)
                splice_seek_restore(in_vf, in_saved);
            vfs_put_file_ref(fd_in, in_vf);
            vfs_put_file_ref(fd_out, out_vf);
            return (int)out_saved;
        }
    }

    size_t total = 0;
    int r = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > SPLICE_CHUNK_SIZE)
            chunk = SPLICE_CHUNK_SIZE;
        r = splice_read_from(in_vf, buf, chunk, nonblock);
        if (r < 0)
            break;
        if (r == 0)
            break;
        int w = splice_write_to(out_vf, buf, (size_t)r, nonblock);
        if (w < 0) {
            r = w;
            break;
        }
        total += (size_t)w;
        if (w < r)
            break;
    }

    if (in_saved >= 0)
        splice_seek_restore(in_vf, in_saved);
    if (out_saved >= 0)
        splice_seek_restore(out_vf, out_saved);
    if (in_pipe)
        splice_pipe_restore_nonblock(in_vf, in_nb);
    if (out_pipe)
        splice_pipe_restore_nonblock(out_vf, out_nb);

    vfs_put_file_ref(fd_in, in_vf);
    vfs_put_file_ref(fd_out, out_vf);

    if (total > 0) {
        if (off_in && !in_pipe)
            *off_in += (long)total;
        if (off_out && !out_pipe)
            *off_out += (long)total;
        return (int)total;
    }
    return r;
}

/* tee(2) core: duplicate up to len bytes from pipe fd_in to pipe fd_out
 * without consuming the source.  Both endpoints must be pipes. */
int tee_do(int fd_in, int fd_out, size_t len, int nonblock)
{
    if (len == 0)
        return 0;

    vfile_t *in_vf = vfs_get_file_ref(fd_in);
    if (!in_vf)
        return -EBADF;
    vfile_t *out_vf = vfs_get_file_ref(fd_out);
    if (!out_vf) {
        vfs_put_file_ref(fd_in, in_vf);
        return -EBADF;
    }

    if (!pipe_vfile_is(in_vf) || !pipe_vfile_is(out_vf)) {
        vfs_put_file_ref(fd_in, in_vf);
        vfs_put_file_ref(fd_out, out_vf);
        return -EINVAL;
    }
    int out_nb = out_vf->flags & O_NONBLOCK;

    char *buf = proc_scratch_buffer(SPLICE_CHUNK_SIZE);
    if (!buf) {
        vfs_put_file_ref(fd_in, in_vf);
        vfs_put_file_ref(fd_out, out_vf);
        return -ENOMEM;
    }

    size_t total = 0;
    int r = 0;
    while (total < len) {
        size_t chunk = len - total;
        if (chunk > SPLICE_CHUNK_SIZE)
            chunk = SPLICE_CHUNK_SIZE;
        r = pipe_peek(in_vf, buf, chunk);
        if (r <= 0)
            break;
        splice_pipe_set_nonblock(out_vf, nonblock);
        int w = vfs_write_file(out_vf, buf, (size_t)r);
        if (w < 0) {
            r = w;
            break;
        }
        total += (size_t)w;
        if (w < r)
            break;
    }

    splice_pipe_restore_nonblock(out_vf, out_nb);
    vfs_put_file_ref(fd_in, in_vf);
    vfs_put_file_ref(fd_out, out_vf);

    return total > 0 ? (int)total : r;
}

/* vmsplice(2) core: copy the user iov segments into the pipe fd.  A20OS
 * pipes are plain ring buffers, so GIFT is accepted as a hint and the bytes
 * are copied, which is always safe for the caller. */
int vmsplice_do(int fd, const void *iov, int nr_segs, int nonblock)
{
    if (nr_segs < 0 || nr_segs > 1024)
        return -EINVAL;

    vfile_t *vf = vfs_get_file_ref(fd);
    if (!vf)
        return -EBADF;
    if (!pipe_vfile_is(vf)) {
        vfs_put_file_ref(fd, vf);
        return -EINVAL;
    }
    int saved_nb = vf->flags & O_NONBLOCK;
    splice_pipe_set_nonblock(vf, nonblock);
    vfs_put_file_ref(fd, vf);

    struct splice_iov {
        char *base;
        size_t len;
    };

    char *kbuf = proc_scratch_buffer(SPLICE_CHUNK_SIZE);
    if (!kbuf) {
        vfile_t *rf = vfs_get_file_ref(fd);
        if (rf)
            splice_pipe_restore_nonblock(rf, saved_nb);
        if (rf)
            vfs_put_file_ref(fd, rf);
        return -ENOMEM;
    }

    size_t total = 0;
    int r = 0;
    for (int i = 0; i < nr_segs; i++) {
        struct splice_iov v;
        if (copy_from_user(&v, (const char *)iov +
                                 (size_t)i * sizeof(struct splice_iov),
                           sizeof(v)) < 0) {
            r = total > 0 ? (int)total : -EFAULT;
            break;
        }
        if (!v.base || v.len == 0)
            continue;
        size_t done = 0;
        while (done < v.len) {
            size_t chunk = v.len - done;
            if (chunk > SPLICE_CHUNK_SIZE)
                chunk = SPLICE_CHUNK_SIZE;
            if (copy_from_user(kbuf, v.base + done, chunk) < 0) {
                r = total > 0 ? (int)total : -EFAULT;
                break;
            }
            vfile_t *wf = vfs_get_file_ref(fd);
            if (!wf) {
                r = total > 0 ? (int)total : -EBADF;
                break;
            }
            int w = vfs_write_file(wf, kbuf, chunk);
            vfs_put_file_ref(fd, wf);
            if (w < 0) {
                r = total > 0 ? (int)total : w;
                break;
            }
            done += (size_t)w;
        }
        if (r != 0)
            break;
        total += v.len;
    }

    vfile_t *rf = vfs_get_file_ref(fd);
    if (rf) {
        splice_pipe_restore_nonblock(rf, saved_nb);
        vfs_put_file_ref(fd, rf);
    }
    return r != 0 ? r : (int)total;
}

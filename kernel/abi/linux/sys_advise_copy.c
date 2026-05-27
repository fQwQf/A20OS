#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"

static int64_t vectored_pread_at(int fd, const void *iov, int iovcnt, long off)
{
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    long saved = vfs_lseek((int)gfd, 0, SEEK_CUR);
    if (saved < 0) return saved;
    if (vfs_lseek((int)gfd, off, SEEK_SET) < 0) return -EINVAL;
    int64_t r = sys_readv(fd, iov, iovcnt);
    vfs_lseek((int)gfd, saved, SEEK_SET);
    return r;
}

static int64_t vectored_pwrite_at(int fd, const void *iov, int iovcnt, long off)
{
    if (iovcnt < 0 || iovcnt > 1024) return -EINVAL;
    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;
    long saved = vfs_lseek((int)gfd, 0, SEEK_CUR);
    if (saved < 0) return saved;
    if (vfs_lseek((int)gfd, off, SEEK_SET) < 0) return -EINVAL;
    int64_t r = sys_writev(fd, iov, iovcnt);
    vfs_lseek((int)gfd, saved, SEEK_SET);
    return r;
}

int64_t sys_preadv(int fd, const void *iov, int iovcnt, long off)
{
    return vectored_pread_at(fd, iov, iovcnt, off);
}

int64_t sys_pwritev(int fd, const void *iov, int iovcnt, long off)
{
    return vectored_pwrite_at(fd, iov, iovcnt, off);
}

int64_t sys_preadv2(int fd, const void *iov, int iovcnt, long off, int flags)
{
    if (flags) return -EOPNOTSUPP;
    return vectored_pread_at(fd, iov, iovcnt, off);
}

int64_t sys_pwritev2(int fd, const void *iov, int iovcnt, long off, int flags)
{
    if (flags) return -EOPNOTSUPP;
    return vectored_pwrite_at(fd, iov, iovcnt, off);
}

int64_t sys_copy_file_range(int fd_in, long *off_in, int fd_out, long *off_out, size_t len, unsigned flags)
{
    if (flags) return -EINVAL;
    if (len == 0) return 0;
    int64_t in_gfd = fdtable_get_current(fd_in);
    int64_t out_gfd = fdtable_get_current(fd_out);
    if (in_gfd < 0) return in_gfd;
    if (out_gfd < 0) return out_gfd;
    long in_off = 0, out_off = 0;
    long in_saved = vfs_lseek((int)in_gfd, 0, SEEK_CUR);
    long out_saved = vfs_lseek((int)out_gfd, 0, SEEK_CUR);
    if (off_in && copy_from_user(&in_off, off_in, sizeof(in_off)) < 0) return -EFAULT;
    if (off_out && copy_from_user(&out_off, off_out, sizeof(out_off)) < 0) return -EFAULT;
    if (off_in) vfs_lseek((int)in_gfd, in_off, SEEK_SET);
    if (off_out) vfs_lseek((int)out_gfd, out_off, SEEK_SET);
    char *buf = proc_scratch_buffer(LINUX_IO_CHUNK_SIZE);
    if (!buf) return -ENOMEM;
    int64_t total = 0;
    while ((size_t)total < len) {
        size_t chunk = len - (size_t)total;
        if (chunk > LINUX_IO_CHUNK_SIZE) chunk = LINUX_IO_CHUNK_SIZE;
        int n = vfs_read((int)in_gfd, buf, chunk);
        if (n <= 0) break;
        int w = vfs_write((int)out_gfd, buf, (size_t)n);
        if (w < 0) { if (!total) total = w; break; }
        total += w;
        if (w < n) break;
    }
    if (off_in) {
        in_off += total > 0 ? total : 0;
        copy_to_user(off_in, &in_off, sizeof(in_off));
        vfs_lseek((int)in_gfd, in_saved, SEEK_SET);
    }
    if (off_out) {
        out_off += total > 0 ? total : 0;
        copy_to_user(off_out, &out_off, sizeof(out_off));
        vfs_lseek((int)out_gfd, out_saved, SEEK_SET);
    }
    return total;
}

int64_t sys_splice(int fd_in, long *off_in, int fd_out, long *off_out, size_t len, unsigned flags)
{
    (void)flags;
    return sys_copy_file_range(fd_in, off_in, fd_out, off_out, len, 0);
}

int64_t sys_vmsplice(int fd, const void *iov, unsigned long nr_segs, unsigned flags)
{
    (void)flags;
    return sys_writev(fd, iov, (int)nr_segs);
}

int64_t sys_tee(int fd_in, int fd_out, size_t len, unsigned flags)
{
    (void)flags;
    int64_t in_gfd = fdtable_get_current(fd_in);
    if (in_gfd < 0) return in_gfd;
    long saved = vfs_lseek((int)in_gfd, 0, SEEK_CUR);
    int64_t r = sys_copy_file_range(fd_in, NULL, fd_out, NULL, len, 0);
    if (saved >= 0) vfs_lseek((int)in_gfd, saved, SEEK_SET);
    return r;
}

int64_t sys_fallocate(int fd, int mode, long off, long len)
{
    if (off < 0 || len < 0) return -EINVAL;
    uint64_t end = (uint64_t)off + (uint64_t)len;
    if (end > 0x7fffffffffffffffULL) return -EFBIG;

    int64_t gfd = fdtable_get_current(fd);
    if (gfd < 0) return gfd;

    vfile_t *vf = vfs_get_file_ref((int)gfd);
    if (!vf) return -EBADF;
    int acc = vf->flags & O_ACCMODE;
    if (acc != O_WRONLY && acc != O_RDWR) {
        vfs_put_file_ref((int)gfd, vf);
        return -EBADF;
    }
    vfs_put_file_ref((int)gfd, vf);

    const int FALLOC_FL_KEEP_SIZE = 0x01;
    if (mode & ~FALLOC_FL_KEEP_SIZE) return -EOPNOTSUPP;
    kstat_t st;
    int r = vfs_fstat((int)gfd, &st);
    if (r < 0) return r;
    if (mode & FALLOC_FL_KEEP_SIZE) return 0;
    if (end > st.st_size) return vfs_ftruncate((int)gfd, end);
    return 0;
}

int64_t sys_posix_fadvise(int fd, long off, long len, int advice)
{
    (void)off; (void)len; (void)advice;
    int64_t gfd = fdtable_get_current(fd);
    return gfd < 0 ? gfd : 0;
}

int64_t sys_close_range(unsigned first, unsigned last, unsigned flags)
{
    const unsigned CLOSE_RANGE_UNSHARE = 1U << 1;
    const unsigned CLOSE_RANGE_CLOEXEC = 1U << 2;
    if (flags & ~(CLOSE_RANGE_UNSHARE | CLOSE_RANGE_CLOEXEC)) return -EINVAL;
    if (first > last) return -EINVAL;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    if (flags & CLOSE_RANGE_UNSHARE) {
        int r = fdtable_unshare(t);
        if (r < 0) return r;
    }
    if (first >= MAX_FILES) return 0;
    if (last >= MAX_FILES) last = MAX_FILES - 1;
    for (unsigned fd = first; fd <= last; fd++) {
        if (fdtable_get(t, (int)fd) < 0)
            continue;
        if (flags & CLOSE_RANGE_CLOEXEC)
            fdtable_set_cloexec(t, (int)fd, 1);
        else
            fdtable_close(t, (int)fd);
    }
    return 0;
}

/*
 * A20OS liba20c — POSIX file, directory, and process wrappers.
 */
#include <unistd.h>
#include <fcntl.h>
#include <string.h>
#include <stdarg.h>
#include <errno.h>
#include <stdio.h>
#include <time.h>
#include "fdtable.h"
#include "a20_fs.h"
#include "a20_handle.h"
#include "a20_task.h"
#include "a20_channel.h"
#include "../liba20rt/a20_syscall.h"

extern a20_start_info_t *a20_get_start_info(void);

#define PATH_MAX 4096

extern int __a20_to_errno(int a20_err);

static char libc_cwd[PATH_MAX + 1];
static a20_handle_t libc_cwd_dir = A20_HANDLE_NULL;
static int libc_cwd_initialized = 0;

static void __cwd_init(void)
{
    if (libc_cwd_initialized)
        return;
    libc_cwd_initialized = 1;

    a20_start_info_t *si = a20_get_start_info();
    if (si) {
        libc_cwd_dir = si->cwd_dir;
        if (libc_cwd_dir == A20_HANDLE_NULL)
            libc_cwd_dir = si->root_dir;
    }
    libc_cwd[0] = '/';
    libc_cwd[1] = '\0';
}

static int __set_errno_status(a20_status_t status)
{
    int err = (int)(-status);
    errno = __a20_to_errno(err);
    return -1;
}

static int __set_errno_a20(int a20_err)
{
    errno = __a20_to_errno(a20_err);
    return -1;
}

static uint32_t __str_len_u32(const char *s)
{
    return (uint32_t)strlen(s);
}

static void __resolve_path(const char *path, char *out, size_t out_size)
{
    __cwd_init();
    if (path[0] == '/') {
        size_t len = strlen(path);
        if (len >= out_size)
            len = out_size - 1;
        memcpy(out, path, len);
        out[len] = '\0';
        return;
    }

    size_t cwd_len = strlen(libc_cwd);
    if (cwd_len > 0 && libc_cwd[cwd_len - 1] == '/') {
        if (cwd_len >= out_size)
            cwd_len = out_size - 1;
        memcpy(out, libc_cwd, cwd_len);
        out[cwd_len] = '\0';
        size_t plen = strlen(path);
        if (cwd_len + plen >= out_size)
            plen = out_size - cwd_len - 1;
        memcpy(out + cwd_len, path, plen);
        out[cwd_len + plen] = '\0';
    } else {
        if (cwd_len >= out_size)
            cwd_len = out_size - 1;
        memcpy(out, libc_cwd, cwd_len);
        out[cwd_len] = '/';
        out[cwd_len + 1] = '\0';
        size_t plen = strlen(path);
        if (cwd_len + 1 + plen >= out_size)
            plen = out_size - cwd_len - 2;
        memcpy(out + cwd_len + 1, path, plen);
        out[cwd_len + 1 + plen] = '\0';
    }
}

static int __normalize_path(const char *path, char *out, size_t out_size)
{
    char tmp[PATH_MAX + 1];
    __resolve_path(path, tmp, sizeof(tmp));

    out[0] = '/';
    out[1] = '\0';
    size_t out_len = 1;
    const char *src = tmp;

    while (*src) {
        while (*src == '/')
            src++;
        if (!*src)
            break;

        const char *end = src;
        while (*end && *end != '/')
            end++;
        size_t len = (size_t)(end - src);

        if (len == 1 && src[0] == '.') {
            src = end;
            continue;
        }
        if (len == 2 && src[0] == '.' && src[1] == '.') {
            if (out_len > 1) {
                char *last = out + out_len - 1;
                while (last > out && *last != '/')
                    last--;
                out_len = (size_t)(last - out);
                if (out_len == 0)
                    out_len = 1;
            }
            src = end;
            continue;
        }

        if (out_len + 1 + len >= out_size) {
            errno = ENAMETOOLONG;
            return -1;
        }
        out[out_len++] = '/';
        memcpy(out + out_len, src, len);
        out_len += len;
        out[out_len] = '\0';
        src = end;
    }

    if (out_len == 1) {
        out[0] = '/';
        out[1] = '\0';
    }
    return 0;
}

int open(const char *path, int flags, ...)
{
    __cwd_init();

    uint32_t rights = A20_RIGHT_STAT;
    int acc = flags & 3;
    if (acc == O_RDONLY)
        rights |= A20_RIGHT_READ;
    else if (acc == O_WRONLY)
        rights |= A20_RIGHT_WRITE;
    else if (acc == O_RDWR)
        rights |= A20_RIGHT_READ | A20_RIGHT_WRITE;

    uint32_t a20_flags = (uint32_t)acc;
    if (flags & O_CREAT)
        a20_flags |= A20_PATH_OPEN_CREATE;
    if (flags & O_TRUNC)
        a20_flags |= A20_PATH_OPEN_TRUNC;
    if (flags & O_APPEND)
        a20_flags |= A20_PATH_OPEN_APPEND;

    uint32_t mode = 0644;
    if (flags & (O_CREAT | O_TMPFILE)) {
        va_list ap;
        va_start(ap, flags);
        mode = va_arg(ap, unsigned int);
        va_end(ap);
    }

    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;

    a20_path_open_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.flags      = a20_flags;
    args.rights     = rights;
    args.path       = (uint64_t)(uintptr_t)full;
    args.path_len   = __str_len_u32(full);
    args.mode       = mode;
    args.out_handle = A20_HANDLE_NULL;

    a20_status_t r = a20_path_open(&args);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);

    int fd = __fd_alloc(args.out_handle);
    if (fd < 0) {
        a20_hdl_close(args.out_handle);
        errno = ENFILE;
        return -1;
    }
    return fd;
}

int close(int fd)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }
    __fd_free(fd);
    a20_syscall6(A20_SYS_handle_close, h, 0, 0, 0, 0, 0);
    return 0;
}

ssize_t read(int fd, void *buf, size_t count)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }

    a20_iovec_t iov;
    iov.base = (uint64_t)(uintptr_t)buf;
    iov.len  = count;

    a20_io_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.handle     = h;
    args._pad0      = 0;
    args.iov        = (uint64_t)&iov;
    args.iov_count  = 1;
    args._pad1      = 0;
    args.offset     = A20_OFFSET_CURRENT;
    args.out_count  = 0;

    a20_status_t r = a20_syscall6(A20_SYS_handle_read, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return -1;
    }
    return (ssize_t)args.out_count;
}

ssize_t write(int fd, const void *buf, size_t count)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }

    a20_iovec_t iov;
    iov.base = (uint64_t)(uintptr_t)buf;
    iov.len  = count;

    a20_io_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.handle     = h;
    args._pad0      = 0;
    args.iov        = (uint64_t)&iov;
    args.iov_count  = 1;
    args._pad1      = 0;
    args.offset     = A20_OFFSET_CURRENT;
    args.out_count  = 0;

    a20_status_t r = a20_syscall6(A20_SYS_handle_write, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return -1;
    }
    return (ssize_t)args.out_count;
}

off_t lseek(int fd, off_t offset, int whence)
{
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }
    a20_off_t off = (a20_off_t)offset;
    a20_status_t r = a20_syscall6(A20_SYS_handle_seek, h,
                                   (uint64_t)&off, (uint64_t)whence, 0, 0, 0);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return -1;
    }
    return (off_t)off;
}

static int __stat_from_handle(a20_handle_t h, struct stat *buf)
{
    a20_stat_t st;
    a20_status_t r = a20_hdl_stat(h, &st);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);

    memset(buf, 0, sizeof(*buf));
    buf->st_dev     = st.dev;
    buf->st_ino     = st.ino;
    buf->st_mode    = st.mode;
    buf->st_nlink   = st.nlink;
    buf->st_uid     = st.uid;
    buf->st_gid     = st.gid;
    buf->st_size    = st.size_bytes;
    buf->st_blksize = 512;
    buf->st_blocks  = st.blocks;
    buf->st_atime   = st.atime_ns;
    buf->st_mtime   = st.mtime_ns;
    buf->st_ctime   = st.ctime_ns;
    return 0;
}

int stat(const char *path, struct stat *buf)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }

    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;

    a20_path_open_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.flags      = 0;
    args.rights     = A20_RIGHT_STAT;
    args.path       = (uint64_t)(uintptr_t)full;
    args.path_len   = __str_len_u32(full);
    args.mode       = 0;
    args.out_handle = A20_HANDLE_NULL;

    a20_status_t r = a20_path_open(&args);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);

    a20_handle_t h = args.out_handle;
    int rc = __stat_from_handle(h, buf);
    a20_hdl_close(h);
    return rc;
}

int fstat(int fd, struct stat *buf)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    uint32_t h = __fd_to_handle(fd);
    if (h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }
    return __stat_from_handle((a20_handle_t)h, buf);
}

int lstat(const char *path, struct stat *buf)
{
    return stat(path, buf);
}

int mkdir(const char *path, mode_t mode)
{
    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;

    a20_path_create_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.type       = A20_OBJ_DIRECTORY;
    args.mode       = (uint32_t)mode;
    args.path       = (uint64_t)(uintptr_t)full;
    args.path_len   = __str_len_u32(full);
    args.dev        = 0;
    args.out_handle = A20_HANDLE_NULL;

    a20_status_t r = a20_path_create(&args);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);
    a20_hdl_close(args.out_handle);
    return 0;
}

int rmdir(const char *path)
{
    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;
    a20_status_t r = a20_path_unlink(A20_HANDLE_NULL, full, __str_len_u32(full));
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);
    return 0;
}

int unlink(const char *path)
{
    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;
    a20_status_t r = a20_path_unlink(A20_HANDLE_NULL, full, __str_len_u32(full));
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);
    return 0;
}

int rename(const char *old, const char *new)
{
    char full_old[PATH_MAX + 1], full_new[PATH_MAX + 1];
    if (__normalize_path(old, full_old, sizeof(full_old)) < 0)
        return -1;
    if (__normalize_path(new, full_new, sizeof(full_new)) < 0)
        return -1;
    a20_status_t r = a20_path_rename(full_old, __str_len_u32(full_old),
                                     full_new, __str_len_u32(full_new));
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);
    return 0;
}

int link(const char *old, const char *new)
{
    char full_old[PATH_MAX + 1], full_new[PATH_MAX + 1];
    if (__normalize_path(old, full_old, sizeof(full_old)) < 0)
        return -1;
    if (__normalize_path(new, full_new, sizeof(full_new)) < 0)
        return -1;
    a20_status_t r = a20_path_link(full_old, __str_len_u32(full_old),
                                   full_new, __str_len_u32(full_new));
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);
    return 0;
}

int symlink(const char *target, const char *linkpath)
{
    char full_link[PATH_MAX + 1];
    if (__normalize_path(linkpath, full_link, sizeof(full_link)) < 0)
        return -1;
    a20_status_t r = a20_path_symlink(target, __str_len_u32(target),
                                      full_link, __str_len_u32(full_link));
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);
    return 0;
}

ssize_t readlink(const char *path, char *buf, size_t bufsiz)
{
    if (!buf) {
        errno = EFAULT;
        return -1;
    }
    if (bufsiz == 0)
        return 0;

    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;
    int64_t r = a20_path_readlink(full, __str_len_u32(full), buf, bufsiz);
    if (r < 0) {
        errno = __a20_to_errno((int)(-r));
        return -1;
    }
    return (ssize_t)r;
}

char *getcwd(char *buf, size_t size)
{
    __cwd_init();
    if (!buf || size == 0) {
        errno = EINVAL;
        return NULL;
    }
    size_t len = strlen(libc_cwd);
    if (len + 1 > size) {
        errno = ERANGE;
        return NULL;
    }
    memcpy(buf, libc_cwd, len + 1);
    return buf;
}

int chdir(const char *path)
{
    __cwd_init();
    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;

    a20_path_open_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.flags      = 0;
    args.rights     = A20_RIGHT_STAT | A20_RIGHT_READ;
    args.path       = (uint64_t)(uintptr_t)full;
    args.path_len   = __str_len_u32(full);
    args.mode       = 0;
    args.out_handle = A20_HANDLE_NULL;

    a20_status_t r = a20_path_open(&args);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);

    struct stat st;
    if (__stat_from_handle(args.out_handle, &st) < 0) {
        a20_hdl_close(args.out_handle);
        return -1;
    }
    if (!S_ISDIR(st.st_mode)) {
        a20_hdl_close(args.out_handle);
        errno = ENOTDIR;
        return -1;
    }

    if (libc_cwd_dir != A20_HANDLE_NULL)
        a20_hdl_close(libc_cwd_dir);
    libc_cwd_dir = args.out_handle;
    strncpy(libc_cwd, full, PATH_MAX);
    libc_cwd[PATH_MAX] = '\0';
    return 0;
}

int dup(int oldfd)
{
    uint32_t h = __fd_to_handle(oldfd);
    if (h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }

    a20_handle_dup_args_t args;
    args.size        = sizeof(args);
    args.version     = 1;
    args.source      = h;
    args.flags       = 0;
    args.rights_mask = A20_RIGHTS_ALL;
    args.out_handle  = A20_HANDLE_NULL;
    args.reserved    = 0;

    a20_status_t r = a20_hdl_dup(&args);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);

    int fd = __fd_alloc(args.out_handle);
    if (fd < 0) {
        a20_hdl_close(args.out_handle);
        errno = ENFILE;
        return -1;
    }
    return fd;
}

int dup2(int oldfd, int newfd)
{
    if (oldfd < 0 || newfd < 0) {
        errno = EBADF;
        return -1;
    }
    if (oldfd == newfd) {
        uint32_t h = __fd_to_handle(oldfd);
        if (h == 0xFFFFFFFF) {
            errno = EBADF;
            return -1;
        }
        return newfd;
    }

    uint32_t old_h = __fd_to_handle(oldfd);
    if (old_h == 0xFFFFFFFF) {
        errno = EBADF;
        return -1;
    }

    a20_handle_dup_args_t args;
    args.size        = sizeof(args);
    args.version     = 1;
    args.source      = old_h;
    args.flags       = 0;
    args.rights_mask = A20_RIGHTS_ALL;
    args.out_handle  = A20_HANDLE_NULL;
    args.reserved    = 0;

    a20_status_t r = a20_hdl_dup(&args);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);

    uint32_t new_h = __fd_to_handle(newfd);
    if (new_h != 0xFFFFFFFF) {
        a20_hdl_close(new_h);
        __fd_free(newfd);
    }
    if (__fd_set(newfd, args.out_handle) < 0) {
        a20_hdl_close(args.out_handle);
        errno = ENFILE;
        return -1;
    }
    return newfd;
}

int pipe(int pipefd[2])
{
    if (!pipefd) {
        errno = EFAULT;
        return -1;
    }

    a20_channel_pair_t pair;
    a20_status_t r = a20_channel_create(&pair);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);

    int rfd = __fd_alloc(pair.endpoints[0]);
    if (rfd < 0) {
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(pair.endpoints[1]);
        errno = ENFILE;
        return -1;
    }
    int wfd = __fd_alloc(pair.endpoints[1]);
    if (wfd < 0) {
        __fd_free(rfd);
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(pair.endpoints[1]);
        errno = ENFILE;
        return -1;
    }
    pipefd[0] = rfd;
    pipefd[1] = wfd;
    return 0;
}

int access(const char *path, int amode)
{
    char full[PATH_MAX + 1];
    if (__normalize_path(path, full, sizeof(full)) < 0)
        return -1;

    if (amode == F_OK) {
        struct stat st;
        return stat(full, &st);
    }

    uint32_t rights = A20_RIGHT_STAT;
    if (amode & R_OK)
        rights |= A20_RIGHT_READ;
    if (amode & W_OK)
        rights |= A20_RIGHT_WRITE;
    if (amode & X_OK)
        rights |= A20_RIGHT_EXEC;

    a20_path_open_args_t args;
    args.size       = sizeof(args);
    args.version    = 1;
    args.dir        = A20_HANDLE_NULL;
    args.flags      = 0;
    args.rights     = rights;
    args.path       = (uint64_t)(uintptr_t)full;
    args.path_len   = __str_len_u32(full);
    args.mode       = 0;
    args.out_handle = A20_HANDLE_NULL;

    a20_status_t r = a20_path_open(&args);
    if (A20_IS_ERROR(r))
        return __set_errno_status(r);
    a20_hdl_close(args.out_handle);
    return 0;
}

unsigned int sleep(unsigned int seconds)
{
    a20_time_t dur;
    dur.secs  = (uint64_t)seconds;
    dur.nsecs = 0;
    a20_status_t r = a20_thread_sleep(dur);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return seconds;
    }
    return 0;
}

int nanosleep(const struct timespec *req, struct timespec *rem)
{
    if (!req) {
        errno = EFAULT;
        return -1;
    }
    if (req->tv_nsec < 0 || req->tv_nsec >= 1000000000L) {
        errno = EINVAL;
        return -1;
    }

    a20_time_t dur;
    dur.secs  = (uint64_t)req->tv_sec;
    dur.nsecs = (uint64_t)req->tv_nsec;
    a20_status_t r = a20_thread_sleep(dur);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return -1;
    }
    if (rem)
        rem->tv_sec = rem->tv_nsec = 0;
    return 0;
}

pid_t getpid(void)
{
    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t task = si ? si->self_task : A20_HANDLE_NULL;
    a20_task_info_t info;
    a20_status_t r = a20_task_info(task, &info);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return -1;
    }
    return (pid_t)info.pid;
}

uid_t getuid(void)
{
    a20_security_context_t ctx;
    ctx.size    = sizeof(ctx);
    ctx.version = 1;
    a20_status_t r = a20_syscall6(A20_SYS_security_get_context,
                                    (uint64_t)&ctx, 0, 0, 0, 0, 0);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return (uid_t)-1;
    }
    return (uid_t)ctx.uid;
}

gid_t getgid(void)
{
    a20_security_context_t ctx;
    ctx.size    = sizeof(ctx);
    ctx.version = 1;
    a20_status_t r = a20_syscall6(A20_SYS_security_get_context,
                                    (uint64_t)&ctx, 0, 0, 0, 0, 0);
    if (A20_IS_ERROR(r)) {
        errno = __a20_to_errno((int)(-r));
        return (gid_t)-1;
    }
    return (gid_t)ctx.gid;
}

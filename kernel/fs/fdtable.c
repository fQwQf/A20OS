#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "core/consts.h"
#include "core/panic.h"
#include "core/string.h"
#include "mm/slab.h"

#define FDTABLE_WORDS ((MAX_FILES + 63) / 64)

static files_struct_t *fdtable_alloc_files(void)
{
    files_struct_t *files = kmalloc(sizeof(*files));
    if (!files)
        panic("fdtable: no memory");
    for (int i = 0; i < MAX_FILES; i++) {
        files->fd[i] = -1;
        files->cloexec[i] = 0;
    }
    for (int i = 0; i < FDTABLE_WORDS; i++)
        files->open_mask[i] = 0;
    files->next_fd = 0;
    refcount_set(&files->refcount, 1);
    return files;
}

static files_struct_t *fdtable_files(task_t *task)
{
    if (!task)
        return NULL;
    if (!task->files)
        task->files = fdtable_alloc_files();
    return (files_struct_t *)task->files;
}

static int fdtable_ref_gfd(int gfd)
{
    return vfs_ref_fd(gfd);
}

static inline void fdtable_mask_set(files_struct_t *files, int fd)
{
    files->open_mask[fd >> 6] |= 1ULL << (fd & 63);
}

static inline void fdtable_mask_clear(files_struct_t *files, int fd)
{
    files->open_mask[fd >> 6] &= ~(1ULL << (fd & 63));
}

static int fdtable_ctz64(uint64_t bits)
{
    int n = 0;
    while ((bits & 1ULL) == 0) {
        bits >>= 1;
        n++;
    }
    return n;
}

static int fdtable_find_free(files_struct_t *files, int minfd)
{
    if (!files)
        return -1;
    if (minfd < 0)
        minfd = 0;
    if (minfd >= MAX_FILES)
        return -1;

    for (int word = minfd >> 6; word < FDTABLE_WORDS; word++) {
        uint64_t used = files->open_mask[word];
        uint64_t free_bits = ~used;
        if (word == (minfd >> 6))
            free_bits &= ~0ULL << (minfd & 63);
        if (word == FDTABLE_WORDS - 1 && (MAX_FILES & 63))
            free_bits &= (1ULL << (MAX_FILES & 63)) - 1;
        if (free_bits)
            return (word << 6) + fdtable_ctz64(free_bits);
    }
    return -1;
}

static int fdtable_fd_limit(task_t *task)
{
    uint64_t limit = task ? task->limits.nofile : MAX_FILES;
    if (limit > MAX_FILES)
        limit = MAX_FILES;
    return (int)limit;
}

static int fdtable_find_free_below(files_struct_t *files, int minfd, int limit)
{
    int fd = fdtable_find_free(files, minfd);
    return (fd >= 0 && fd < limit) ? fd : -1;
}

static void fdtable_note_alloc(files_struct_t *files, int fd)
{
    if (!files)
        return;
    fdtable_mask_set(files, fd);
    if (fd >= files->next_fd)
        files->next_fd = fdtable_find_free(files, fd + 1);
}

static void fdtable_note_free(files_struct_t *files, int fd)
{
    if (!files || fd < 0 || fd >= MAX_FILES)
        return;
    fdtable_mask_clear(files, fd);
    if (files->next_fd < 0 || fd < files->next_fd)
        files->next_fd = fd;
}

static void fdtable_recompute_next(files_struct_t *files)
{
    if (!files)
        return;
    for (int i = 0; i < FDTABLE_WORDS; i++)
        files->open_mask[i] = 0;
    for (int fd = 0; fd < MAX_FILES; fd++) {
        if (files->fd[fd] >= 0)
            fdtable_mask_set(files, fd);
    }
    files->next_fd = fdtable_find_free(files, 0);
}

void fdtable_init(task_t *task)
{
    if (!task)
        return;
    if (task->files)
        kfree(task->files);
    task->files = fdtable_alloc_files();
    fdtable_init_stdio(task);
}

void fdtable_init_stdio(task_t *task)
{
    files_struct_t *files = fdtable_files(task);
    if (!files)
        return;
    for (int fd = 0; fd < 3 && fd < MAX_FILES; fd++) {
        if (files->fd[fd] >= 0)
            continue;
        files->fd[fd] = fd;
        files->cloexec[fd] = 0;
        fdtable_note_alloc(files, fd);
        fdtable_ref_gfd(fd);
    }
}

void fdtable_copy(task_t *dst, const task_t *src)
{
    if (!dst)
        return;
    if (dst->files)
        kfree(dst->files);
    dst->files = fdtable_alloc_files();
    if (!src) {
        fdtable_init_stdio(dst);
        return;
    }
    const files_struct_t *src_files = (const files_struct_t *)src->files;
    files_struct_t *dst_files = (files_struct_t *)dst->files;
    if (!src_files) {
        fdtable_init_stdio(dst);
        return;
    }
    for (int i = 0; i < MAX_FILES; i++) {
        int gfd = src_files->fd[i];
        dst_files->fd[i] = gfd;
        dst_files->cloexec[i] = src_files->cloexec[i];
        if (gfd >= 0)
            fdtable_ref_gfd(gfd);
    }
    fdtable_recompute_next(dst_files);
    fdtable_init_stdio(dst);
}

void fdtable_share(task_t *dst, const task_t *src)
{
    if (!dst || !src)
        return;
    if (dst->files)
        kfree(dst->files);
    dst->files = (struct files_struct *)src->files;
    if (dst->files)
        refcount_inc(&((files_struct_t *)dst->files)->refcount);
}

int fdtable_unshare(task_t *task)
{
    if (!task)
        return -ESRCH;
    files_struct_t *old = fdtable_files(task);
    if (!old)
        return -ENOMEM;
    if (refcount_read(&old->refcount) == 1)
        return 0;

    files_struct_t *files = fdtable_alloc_files();
    for (int i = 0; i < MAX_FILES; i++) {
        int gfd = old->fd[i];
        files->fd[i] = gfd;
        files->cloexec[i] = old->cloexec[i];
        if (gfd >= 0) {
            int r = fdtable_ref_gfd(gfd);
            if (r < 0) {
                for (int j = 0; j < i; j++) {
                    if (files->fd[j] >= 0)
                        vfs_close(files->fd[j]);
                }
                kfree(files);
                return r;
            }
        }
    }
    fdtable_recompute_next(files);
    task->files = files;
    refcount_dec_and_test(&old->refcount);
    return 0;
}

void fdtable_close_all(task_t *task)
{
    if (!task || !task->files)
        return;
    files_struct_t *files = (files_struct_t *)task->files;
    task->files = NULL;
    if (!refcount_dec_and_test(&files->refcount))
        return;
    for (int i = 0; i < MAX_FILES; i++) {
        if (files->fd[i] >= 0)
            vfs_close(files->fd[i]);
        files->fd[i] = -1;
        files->cloexec[i] = 0;
    }
    for (int i = 0; i < FDTABLE_WORDS; i++)
        files->open_mask[i] = 0;
    kfree(files);
}

void fdtable_close_on_exec(task_t *task)
{
    files_struct_t *files = fdtable_files(task);
    if (!files)
        return;
    for (int i = 0; i < MAX_FILES; i++) {
        if (files->cloexec[i] && files->fd[i] >= 0) {
            vfs_close(files->fd[i]);
            files->fd[i] = -1;
            fdtable_note_free(files, i);
        }
        files->cloexec[i] = 0;
    }
    fdtable_recompute_next(files);
    fdtable_init_stdio(task);
}

int fdtable_get(task_t *task, int fd)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    int gfd = files->fd[fd];
    if (gfd < 0)
        return -EBADF;
    return gfd;
}

int fdtable_get_current(int fd)
{
    return fdtable_get(proc_current(), fd);
}

int fdtable_install(task_t *task, int gfd, int flags)
{
    if (!task || gfd < 0)
        return -EBADF;
    files_struct_t *files = fdtable_files(task);
    if (!files)
        return -ESRCH;
    int limit = fdtable_fd_limit(task);
    int fd = fdtable_find_free_below(files, files->next_fd, limit);
    if (fd < 0)
        fd = fdtable_find_free_below(files, 0, limit);
    if (fd >= 0) {
        files->fd[fd] = gfd;
        files->cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
        fdtable_note_alloc(files, fd);
        return fd;
    }
    vfs_close(gfd);
    return -EMFILE;
}

int fdtable_install_current(int gfd, int flags)
{
    return fdtable_install(proc_current(), gfd, flags);
}

int fdtable_close(task_t *task, int fd)
{
    int gfd = fdtable_get(task, fd);
    if (gfd < 0)
        return gfd;
    files_struct_t *files = (files_struct_t *)task->files;
    files->fd[fd] = -1;
    files->cloexec[fd] = 0;
    fdtable_note_free(files, fd);
    return vfs_close(gfd);
}

int fdtable_close_current(int fd)
{
    return fdtable_close(proc_current(), fd);
}

int fdtable_dup(task_t *task, int oldfd, int minfd, int flags)
{
    if (!task)
        return -ESRCH;
    files_struct_t *files = (files_struct_t *)task->files;
    if (!files)
        return -EBADF;
    if (flags & ~O_CLOEXEC)
        return -EINVAL;
    if (oldfd < 0 || oldfd >= MAX_FILES)
        return -EBADF;
    if (minfd < 0)
        minfd = 0;
    int limit = fdtable_fd_limit(task);
    if (minfd >= limit)
        return -EMFILE;

    int gfd = files->fd[oldfd];
    if (gfd < 0)
        return -EBADF;
    if (fdtable_ref_gfd(gfd) < 0)
        return -EBADF;

    int fd = fdtable_find_free_below(files, minfd, limit);
    if (fd >= 0) {
        files->fd[fd] = gfd;
        files->cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
        fdtable_note_alloc(files, fd);
        return fd;
    }
    vfs_close(gfd);
    return -EMFILE;
}

int fdtable_dup_to(task_t *task, int oldfd, int newfd, int flags)
{
    if (!task)
        return -ESRCH;
    files_struct_t *files = (files_struct_t *)task->files;
    if (!files)
        return -EBADF;
    if (flags & ~O_CLOEXEC)
        return -EINVAL;
    if (oldfd < 0 || oldfd >= MAX_FILES || newfd < 0 || newfd >= MAX_FILES)
        return -EBADF;
    if (newfd >= fdtable_fd_limit(task))
        return -EBADF;
    if (oldfd == newfd)
        return -EINVAL;

    int gfd = files->fd[oldfd];
    if (gfd < 0)
        return -EBADF;

    if (files->fd[newfd] >= 0)
        vfs_close(files->fd[newfd]);
    if (fdtable_ref_gfd(gfd) < 0) {
        files->fd[newfd] = -1;
        files->cloexec[newfd] = 0;
        fdtable_note_free(files, newfd);
        return -EBADF;
    }
    files->fd[newfd] = gfd;
    files->cloexec[newfd] = (flags & O_CLOEXEC) ? 1 : 0;
    fdtable_note_alloc(files, newfd);
    return newfd;
}

int fdtable_get_cloexec(task_t *task, int fd)
{
    if (fdtable_get(task, fd) < 0)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    return files->cloexec[fd] ? FD_CLOEXEC : 0;
}

int fdtable_set_cloexec(task_t *task, int fd, int cloexec)
{
    if (fdtable_get(task, fd) < 0)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    files->cloexec[fd] = cloexec ? 1 : 0;
    return 0;
}

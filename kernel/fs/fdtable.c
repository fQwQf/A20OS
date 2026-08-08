#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "proc/proc_internal.h"
#include "core/consts.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/klog.h"
#include "mm/slab.h"

#define FDTABLE_WORDS ((MAX_FILES + 63) / 64)

static int fdtable_ctz64(uint64_t bits);

static files_struct_t *fdtable_alloc_files(void)
{
    files_struct_t *files = kmalloc(sizeof(*files));
    if (!files)
        panic("fdtable: no memory");
    spin_init(&files->lock);
    spin_set_debug(&files->lock, "files", files);
    memset(files->fd, 0xff, sizeof(files->fd));
    memset(files->cloexec, 0, sizeof(files->cloexec));
    memset(files->open_mask, 0, sizeof(files->open_mask));
    files->next_fd = 0;
    refcount_set(&files->refcount, 1);
    files->owners = 1;
    files->release_owner_pid = -1;
    ktrace_fd("[FDDBG] files=%p lock=%p\n", (void *)files, (void *)&files->lock);
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

static void fdtable_files_put(files_struct_t *files)
{
    if (!files || !refcount_dec_and_test(&files->refcount))
        return;

    int to_close[MAX_FILES];
    int close_count = 0;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    for (int word = 0; word < FDTABLE_WORDS; word++) {
        uint64_t open = files->open_mask[word];
        while (open) {
            int bit = fdtable_ctz64(open);
            int fd = (word << 6) + bit;
            open &= open - 1;
            if (fd >= MAX_FILES)
                break;
            to_close[close_count++] = files->fd[fd];
            files->fd[fd] = -1;
            files->cloexec[fd] = 0;
        }
        files->open_mask[word] = 0;
    }
    spin_unlock_irqrestore(&files->lock, flags);

    for (int i = 0; i < close_count; i++) {
        if (files->release_owner_pid >= 0)
            vfs_release_process_file_locks(to_close[i],
                                           files->release_owner_pid);
        vfs_close(to_close[i]);
    }
    kfree(files);
}

static int fdtable_ctz64(uint64_t bits)
{
    if (bits == 0) return 64;
    int n = 0;
    if ((bits & 0xFFFFFFFF) == 0) { n += 32; bits >>= 32; }
    if ((bits & 0xFFFF) == 0)     { n += 16; bits >>= 16; }
    if ((bits & 0xFF) == 0)       { n += 8;  bits >>= 8;  }
    if ((bits & 0xF) == 0)        { n += 4;  bits >>= 4;  }
    if ((bits & 0x3) == 0)        { n += 2;  bits >>= 2;  }
    if ((bits & 0x1) == 0)        { n += 1; }
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
    files_struct_t *src_files = (files_struct_t *)src->files;
    files_struct_t *dst_files = (files_struct_t *)dst->files;
    if (!src_files) {
        fdtable_init_stdio(dst);
        return;
    }
    uint64_t flags = spin_lock_irqsave(&src_files->lock);
    memcpy(dst_files->open_mask, src_files->open_mask,
           sizeof(dst_files->open_mask));
    dst_files->next_fd = src_files->next_fd;
    for (int word = 0; word < FDTABLE_WORDS; word++) {
        uint64_t open = src_files->open_mask[word];
        while (open) {
            int bit = fdtable_ctz64(open);
            int fd = (word << 6) + bit;
            open &= open - 1;
            if (fd >= MAX_FILES)
                break;
            int gfd = src_files->fd[fd];
            dst_files->fd[fd] = gfd;
            dst_files->cloexec[fd] = src_files->cloexec[fd];
            if (gfd < 0 || fdtable_ref_gfd(gfd) < 0)
                panic("fdtable_copy: open local fd %d has dead gfd %d", fd, gfd);
        }
    }
    spin_unlock_irqrestore(&src_files->lock, flags);
}

void fdtable_share(task_t *dst, const task_t *src)
{
    if (!dst || !src)
        return;
    if (dst->files)
        fdtable_close_all(dst);
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    dst->files = (struct files_struct *)src->files;
    if (dst->files) {
        files_struct_t *files = (files_struct_t *)dst->files;
        refcount_inc(&files->refcount);
        files->owners++;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
}

int fdtable_unshare(task_t *task)
{
    if (!task)
        return -ESRCH;
    files_struct_t *old = fdtable_files(task);
    if (!old)
        return -ENOMEM;
    uint64_t owner_flags = spin_lock_irqsave(&proc_lock);
    int shared = old->owners > 1;
    spin_unlock_irqrestore(&proc_lock, owner_flags);
    if (!shared)
        return 0;

    files_struct_t *files = fdtable_alloc_files();
    uint64_t flags = spin_lock_irqsave(&old->lock);
    for (int word = 0; word < FDTABLE_WORDS; word++) {
        uint64_t open = old->open_mask[word];
        while (open) {
            int bit = fdtable_ctz64(open);
            int fd = (word << 6) + bit;
            open &= open - 1;
            if (fd >= MAX_FILES)
                break;
            int gfd = old->fd[fd];
            if (gfd < 0) {
                spin_unlock_irqrestore(&old->lock, flags);
                fdtable_files_put(files);
                return -EBADF;
            }
            files->fd[fd] = gfd;
            files->cloexec[fd] = old->cloexec[fd];
            int r = fdtable_ref_gfd(gfd);
            if (r < 0) {
                files->fd[fd] = -1;
                spin_unlock_irqrestore(&old->lock, flags);
                fdtable_files_put(files);
                return r;
            }
            fdtable_mask_set(files, fd);
        }
    }
    files->next_fd = old->next_fd;
    spin_unlock_irqrestore(&old->lock, flags);
    uint64_t task_flags = spin_lock_irqsave(&proc_lock);
    task->files = files;
    old->owners--;
    spin_unlock_irqrestore(&proc_lock, task_flags);
    fdtable_files_put(old);
    return 0;
}

void fdtable_close_all(task_t *task)
{
    if (!task || !task->files)
        return;
    uint64_t flags = spin_lock_irqsave(&proc_lock);
    files_struct_t *files = (files_struct_t *)task->files;
    task->files = NULL;
    if (files) {
        files->owners--;
        if (files->owners == 0)
            files->release_owner_pid = task->pid;
    }
    spin_unlock_irqrestore(&proc_lock, flags);
    fdtable_files_put(files);
}

void fdtable_close_on_exec(task_t *task)
{
    files_struct_t *files = fdtable_files(task);
    if (!files)
        return;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int to_close[MAX_FILES];
    int close_count = 0;
    for (int word = 0; word < FDTABLE_WORDS; word++) {
        uint64_t open = files->open_mask[word];
        while (open) {
            int bit = fdtable_ctz64(open);
            int fd = (word << 6) + bit;
            open &= open - 1;
            if (fd >= MAX_FILES)
                break;
            if (files->cloexec[fd]) {
                to_close[close_count++] = files->fd[fd];
                files->fd[fd] = -1;
                files->cloexec[fd] = 0;
                fdtable_note_free(files, fd);
            }
        }
    }
    spin_unlock_irqrestore(&files->lock, flags);
    for (int i = 0; i < close_count; i++) {
        vfs_release_process_file_locks(to_close[i], task->pid);
        vfs_close(to_close[i]);
    }
    fdtable_init_stdio(task);
}

int fdtable_get(task_t *task, int fd)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int gfd = files->fd[fd];
    if (gfd < 0) {
        spin_unlock_irqrestore(&files->lock, flags);
        return -EBADF;
    }
    spin_unlock_irqrestore(&files->lock, flags);
    return gfd;
}

int fdtable_get_current(int fd)
{
    return fdtable_get(proc_current(), fd);
}

struct vfile *fdtable_get_file_ref(task_t *task, int fd, int *gfd_out,
                                   int *cloexec_out)
{
    if (!task || fd < 0 || fd >= MAX_FILES)
        return NULL;

    uint64_t task_flags = spin_lock_irqsave(&proc_lock);
    files_struct_t *files = (files_struct_t *)task->files;
    if (files && !refcount_inc_not_zero(&files->refcount))
        files = NULL;
    spin_unlock_irqrestore(&proc_lock, task_flags);
    if (!files)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&files->lock);
    int gfd = files->fd[fd];
    int cloexec = files->cloexec[fd] != 0;
    vfile_t *vf = gfd >= 0 ? vfs_get_file_ref(gfd) : NULL;
    spin_unlock_irqrestore(&files->lock, flags);

    fdtable_files_put(files);
    if (!vf)
        return NULL;
    if (gfd_out)
        *gfd_out = gfd;
    if (cloexec_out)
        *cloexec_out = cloexec;
    return vf;
}

vfile_t *fdtable_get_current_file_ref(int fd, int *gfd_out)
{
    return fdtable_get_file_ref(proc_current(), fd, gfd_out, NULL);
}

int fdtable_install(task_t *task, int gfd, int flags)
{
    if (!task || gfd < 0)
        return -EBADF;
    files_struct_t *files = fdtable_files(task);
    if (!files)
        return -ESRCH;
    uint64_t lock_flags = spin_lock_irqsave(&files->lock);
    int limit = fdtable_fd_limit(task);
    int fd = fdtable_find_free_below(files, files->next_fd, limit);
    if (fd < 0)
        fd = fdtable_find_free_below(files, 0, limit);
    if (fd >= 0) {
        files->fd[fd] = gfd;
        files->cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
        fdtable_note_alloc(files, fd);
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return fd;
    }
    spin_unlock_irqrestore(&files->lock, lock_flags);
    vfs_close(gfd);
    return -EMFILE;
}

int fdtable_install_current(int gfd, int flags)
{
    return fdtable_install(proc_current(), gfd, flags);
}

int fdtable_close(task_t *task, int fd)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int gfd = files->fd[fd];
    if (gfd < 0) {
        spin_unlock_irqrestore(&files->lock, flags);
        return -EBADF;
    }
    files->fd[fd] = -1;
    files->cloexec[fd] = 0;
    fdtable_note_free(files, fd);
    spin_unlock_irqrestore(&files->lock, flags);
    ktrace_fd("[FD] close: pid=%d lfd=%d gfd=%d\n", task->pid, fd, gfd);
    vfs_release_process_file_locks(gfd, task->pid);
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
    uint64_t lock_flags = spin_lock_irqsave(&files->lock);
    int limit = fdtable_fd_limit(task);
    if (minfd >= limit) {
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return -EMFILE;
    }

    int gfd = files->fd[oldfd];
    if (gfd < 0) {
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return -EBADF;
    }
    if (fdtable_ref_gfd(gfd) < 0) {
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return -EBADF;
    }

    int fd = fdtable_find_free_below(files, minfd, limit);
    if (fd >= 0) {
        files->fd[fd] = gfd;
        files->cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
        fdtable_note_alloc(files, fd);
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return fd;
    }
    spin_unlock_irqrestore(&files->lock, lock_flags);
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

    uint64_t lock_flags = spin_lock_irqsave(&files->lock);
    int gfd = files->fd[oldfd];
    if (gfd < 0) {
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return -EBADF;
    }

    int old_new_gfd = files->fd[newfd];
    files->fd[newfd] = -1;
    files->cloexec[newfd] = 0;
    fdtable_note_free(files, newfd);
    if (fdtable_ref_gfd(gfd) < 0) {
        spin_unlock_irqrestore(&files->lock, lock_flags);
        if (old_new_gfd >= 0)
            vfs_close(old_new_gfd);
        return -EBADF;
    }
    files->fd[newfd] = gfd;
    files->cloexec[newfd] = (flags & O_CLOEXEC) ? 1 : 0;
    fdtable_note_alloc(files, newfd);
    spin_unlock_irqrestore(&files->lock, lock_flags);
    if (old_new_gfd >= 0) {
        vfs_release_process_file_locks(old_new_gfd, task->pid);
        vfs_close(old_new_gfd);
    }
    return newfd;
}

int fdtable_get_cloexec(task_t *task, int fd)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int ret = files->fd[fd] >= 0 ? (files->cloexec[fd] ? FD_CLOEXEC : 0) : -EBADF;
    spin_unlock_irqrestore(&files->lock, flags);
    return ret;
}

int fdtable_set_cloexec(task_t *task, int fd, int cloexec)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    if (files->fd[fd] < 0) {
        spin_unlock_irqrestore(&files->lock, flags);
        return -EBADF;
    }
    files->cloexec[fd] = cloexec ? 1 : 0;
    spin_unlock_irqrestore(&files->lock, flags);
    return 0;
}

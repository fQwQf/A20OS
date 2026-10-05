#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "fs/devfs.h"
#include "fs/vfs/mntns.h"
#include "ipc/envelope.h"
#include "proc/proc_internal.h"
#include "core/consts.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/klog.h"
#include "mm/slab.h"

#define FDTABLE_WORDS ((MAX_FILES + 63) / 64)

static int fdtable_ctz64(uint64_t bits);
static inline void fdtable_mask_set(files_struct_t *files, int fd);
static int fdtable_find_free(files_struct_t *files, int minfd);

/* fd slots reference vfiles directly.  Closing a slot drops that reference;
 * when it is the last one the finalizer (ops->close + vfile_free +
 * vnode_put) must run.  The finalizer lives in vfs.c; fdtable.c is the only
 * caller outside vfs.c's own close paths. */
void vfs_finalize_closed_vfile(vfile_t *vf);

/* Pinned for the life of the kernel: fd operations with no current task
 * resolve here.  It never takes part in the refcount and is never torn
 * down, so fdtable_files_put() treats it specially. */
static files_struct_t fdtable_boot_files;
static bool fdtable_boot_stdio_done;

static files_struct_t *fdtable_alloc_files(void)
{
    files_struct_t *files = kmalloc(sizeof(*files));
    if (!files)
        panic("fdtable: no memory");
    spin_init(&files->lock);
    spin_set_debug(&files->lock, "files", files);
    wait_queue_init(&files->readiness_waiters);
    memset(files->fd, 0, sizeof(files->fd));
    memset(files->cloexec, 0, sizeof(files->cloexec));
    memset(files->open_mask, 0, sizeof(files->open_mask));
    files->next_fd = 0;
    refcount_set(&files->refcount, 1);
    files->owners = 1;
    files->release_owner_pid = -1;
    ktrace_fd("[FDDBG] files=%p lock=%p\n", (void *)files, (void *)&files->lock);
    return files;
}

/* Table that fd operations apply to when the caller has no current task:
 * the boot table, initialised on first use with stdio attached. */
static files_struct_t *fdtable_boot(void)
{
    if (!fdtable_boot_stdio_done) {
        spin_lock(&fdtable_boot_files.lock);
        if (!fdtable_boot_stdio_done) {
            files_struct_t *files = &fdtable_boot_files;
            for (int fd = 0; fd < 3 && fd < MAX_FILES; fd++) {
                vfile_t *s = devfs_create_stdio(fd);
                if (!s)
                    break;
                files->fd[fd] = s;
                files->cloexec[fd] = 0;
                fdtable_mask_set(files, fd);
                vfile_get(s);
                if (fd >= files->next_fd)
                    files->next_fd = fdtable_find_free(files, fd + 1);
            }
            fdtable_boot_stdio_done = true;
        }
        spin_unlock(&fdtable_boot_files.lock);
    }
    return &fdtable_boot_files;
}

static files_struct_t *fdtable_files(task_t *task)
{
    if (!task)
        return NULL;
    if (!task->files)
        task->files = fdtable_alloc_files();
    return (files_struct_t *)task->files;
}

/* The table fd operations resolve against: the current task's, or the boot
 * table when there is no current task (boot, IRQ before first task). */
static files_struct_t *fdtable_active_files(task_t *task)
{
    if (task)
        return fdtable_files(task);
    task_t *cur = proc_current();
    if (cur)
        return fdtable_files(cur);
    return fdtable_boot();
}

static inline void fdtable_mask_set(files_struct_t *files, int fd)
{
    files->open_mask[fd >> 6] |= 1ULL << (fd & 63);
}

static inline void fdtable_mask_clear(files_struct_t *files, int fd)
{
    files->open_mask[fd >> 6] &= ~(1ULL << (fd & 63));
}

/* Drop one fd-slot reference and run the finalizer when it was the last. */
static void fdtable_slot_put(vfile_t *vf)
{
    if (!vf)
        return;
    if (vfile_put_ref_only(vf))
        vfs_finalize_closed_vfile(vf);
}

static void fdtable_files_put(files_struct_t *files)
{
    if (!files || !refcount_dec_and_test(&files->refcount))
        return;

    vfile_t *to_close[MAX_FILES];
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
            files->fd[fd] = NULL;
            files->cloexec[fd] = 0;
            env_kind_unregister(fd);
        }
        files->open_mask[word] = 0;
    }
    spin_unlock_irqrestore(&files->lock, flags);

    if (files == &fdtable_boot_files)
        return; /* pinned forever; its stdio slots are never dropped */
    for (int i = 0; i < close_count; i++) {
        if (files->release_owner_pid >= 0)
            vfs_release_process_file_locks(to_close[i],
                                           files->release_owner_pid);
        fdtable_slot_put(to_close[i]);
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
        if (files->fd[fd])
            continue;
        vfile_t *s = devfs_create_stdio(fd);
        if (!s)
            continue;
        files->fd[fd] = s;
        files->cloexec[fd] = 0;
        fdtable_note_alloc(files, fd);
        vfile_get(s);
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
            vfile_t *vf = src_files->fd[fd];
            dst_files->fd[fd] = vf;
            dst_files->cloexec[fd] = src_files->cloexec[fd];
            if (!vf)
                panic("fdtable_copy: open local fd %d has no vfile", fd);
            vfile_get(vf);
        }
    }
    spin_unlock_irqrestore(&src_files->lock, flags);
}

void fdtable_share(task_t *dst, const task_t *src)
{
    if (!dst || !src)
        return;
    /* Only the descriptor table is being replaced.  @dst is a live, already
     * initialised task: the namespace references and the fs pins it was born
     * with were taken by proc_task_init_common() and are still in use, so this
     * must go through fdtable_release_files() rather than the full per-task
     * teardown that fdtable_close_all() performs. */
    if (dst->files)
        fdtable_release_files(dst);
    /* ->files is published with the task's other per-task state, so the
     * install is done under the target's park_lock. */
    uint64_t flags = spin_lock_irqsave(&dst->park_lock);
    dst->files = (struct files_struct *)src->files;
    if (dst->files) {
        files_struct_t *files = (files_struct_t *)dst->files;
        refcount_inc(&files->refcount);
        files->owners++;
    }
    spin_unlock_irqrestore(&dst->park_lock, flags);
}

int fdtable_unshare(task_t *task)
{
    if (!task)
        return -ESRCH;
    files_struct_t *old = fdtable_files(task);
    if (!old)
        return -ENOMEM;
    uint64_t owner_flags = spin_lock_irqsave(&old->lock);
    int shared = old->owners > 1;
    spin_unlock_irqrestore(&old->lock, owner_flags);
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
            vfile_t *vf = old->fd[fd];
            if (!vf) {
                spin_unlock_irqrestore(&old->lock, flags);
                fdtable_files_put(files);
                return -EBADF;
            }
            files->fd[fd] = vf;
            files->cloexec[fd] = old->cloexec[fd];
            vfile_get(vf);
            fdtable_mask_set(files, fd);
        }
    }
    files->next_fd = old->next_fd;
    spin_unlock_irqrestore(&old->lock, flags);
    /* The swap is the task's own park_lock critical section; the owner count
     * on the retired table stays under that table's lock. */
    uint64_t task_flags = spin_lock_irqsave(&task->park_lock);
    task->files = files;
    spin_unlock_irqrestore(&task->park_lock, task_flags);
    uint64_t old_flags = spin_lock_irqsave(&old->lock);
    old->owners--;
    spin_unlock_irqrestore(&old->lock, old_flags);
    fdtable_files_put(old);
    return 0;
}

void fdtable_release_files(task_t *task)
{
    if (!task || !task->files)
        return;
    uint64_t flags = spin_lock_irqsave(&task->park_lock);
    files_struct_t *files = (files_struct_t *)task->files;
    task->files = NULL;
    if (files) {
        files->owners--;
        if (files->owners == 0)
            files->release_owner_pid = task->pid;
    }
    spin_unlock_irqrestore(&task->park_lock, flags);
    fdtable_files_put(files);
}

void fdtable_close_all(task_t *task)
{
    if (!task)
        return;
    /* Genuine per-task teardown, reached from process exit and from
     * proc_destroy_task().  It drops the mount-namespace reference, which
     * mntns_release_task() NULLs, so both paths reaching it is safe. */
    mntns_release_task(task);
    /* Same hook for the pid namespace: the task's ids in every container it
     * is a member of must come back as soon as the task is gone, or a long
     * running container would leak ids until PIDNS_CHILD_MAX. */
    pidns_release_task(task);
    /* And the user namespace reference, on the same reasoning: a namespace
     * that outlives its last task would keep its parent pinned. */
    userns_release_task(task);
    /* Release the per-process root/cwd references.  Idempotent, so the
     * exit and destroy paths that both reach this hook are safe. */
    vfs_task_fs_pins_release(task);
    /* The descriptor table itself is the other half, and is separable: a
     * task that is giving its table away rather than going away wants the
     * files without the namespaces (see fdtable_share()). */
    fdtable_release_files(task);
}

void fdtable_close_on_exec(task_t *task)
{
    files_struct_t *files = fdtable_files(task);
    if (!files)
        return;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    vfile_t *to_close[MAX_FILES];
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
                files->fd[fd] = NULL;
                files->cloexec[fd] = 0;
                fdtable_note_free(files, fd);
                /* Slot is free for reuse; a class entry left behind would be
                 * read by env_kind_of() for whatever lands here next. */
                env_kind_unregister(fd);
            }
        }
    }
    spin_unlock_irqrestore(&files->lock, flags);
    if (close_count)
        wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    for (int i = 0; i < close_count; i++) {
        vfs_release_process_file_locks(to_close[i], task->pid);
        fdtable_slot_put(to_close[i]);
    }
    fdtable_init_stdio(task);
}

int fdtable_get(task_t *task, int fd)
{
    if (fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files;
    if (task)
        files = (files_struct_t *)task->files;
    else {
        task_t *cur = proc_current();
        if (cur)
            files = (files_struct_t *)cur->files;
        else
            files = fdtable_boot();
    }
    if (!files)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int r = files->fd[fd] ? fd : -EBADF;
    spin_unlock_irqrestore(&files->lock, flags);
    return r;
}

int fdtable_get_current(int fd)
{
    return fdtable_get(proc_current(), fd);
}

wait_queue_t *fdtable_current_readiness_queue(void)
{
    task_t *task = proc_current();
    files_struct_t *files = task ? (files_struct_t *)task->files
                                 : fdtable_boot();
    return files ? &files->readiness_waiters : NULL;
}

bool fdtable_current_matches_file(int fd, vfile_t *vf, uint64_t identity)
{
    vfile_t *current = fdtable_get_current_file_ref(fd);
    bool matches = current && current == vf && current->identity == identity;
    if (current)
        vfs_put_file(current);
    return matches;
}

struct vfile *fdtable_get_file_ref(task_t *task, int fd, int *cloexec_out)
{
    if (fd < 0 || fd >= MAX_FILES)
        return NULL;

    if (!task) {
        task_t *cur = proc_current();
        if (cur)
            task = cur;
    }

    files_struct_t *files;
    if (task) {
        uint64_t task_flags = spin_lock_irqsave(&task->park_lock);
        files = (files_struct_t *)task->files;
        if (files && !refcount_inc_not_zero(&files->refcount))
            files = NULL;
        spin_unlock_irqrestore(&task->park_lock, task_flags);
    } else {
        files = fdtable_boot(); /* pinned: no refcount games */
    }
    if (!files)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&files->lock);
    vfile_t *vf = files->fd[fd];
    int cloexec = files->cloexec[fd] != 0;
    if (vf)
        vfile_get(vf);
    spin_unlock_irqrestore(&files->lock, flags);

    fdtable_files_put(files);
    if (!vf)
        return NULL;
    if (cloexec_out)
        *cloexec_out = cloexec;
    return vf;
}

vfile_t *fdtable_get_current_file_ref(int fd)
{
    return fdtable_get_file_ref(proc_current(), fd, NULL);
}

int fdtable_install_vfile(task_t *task, vfile_t *vf, int flags)
{
    if (!vf)
        return -EBADF;
    files_struct_t *files = fdtable_active_files(task);
    if (!files)
        return -ESRCH;
    uint64_t lock_flags = spin_lock_irqsave(&files->lock);
    int limit = fdtable_fd_limit(task);
    int fd = fdtable_find_free_below(files, files->next_fd, limit);
    if (fd < 0)
        fd = fdtable_find_free_below(files, 0, limit);
    if (fd >= 0) {
        files->fd[fd] = vf;
        files->cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
        fdtable_note_alloc(files, fd);
        /* Reference semantics: the slot TAKES OVER the caller's reference.
         * On failure the caller keeps it.  A close then drops exactly one
         * reference per slot. */
        spin_unlock_irqrestore(&files->lock, lock_flags);
        wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
        return fd;
    }
    spin_unlock_irqrestore(&files->lock, lock_flags);
    return -EMFILE;
}

int fdtable_install_current_vfile(vfile_t *vf, int flags)
{
    return fdtable_install_vfile(proc_current(), vf, flags);
}

int fdtable_close(task_t *task, int fd)
{
    if (!task || !task->files)
        return -EBADF;
    return fdtable_close_files((files_struct_t *)task->files, fd, task->pid);
}

/*
 * Release one descriptor slot in @files.  Split out of fdtable_close() because
 * a descriptor opened with no current task lands in the boot table rather than
 * in any task's files_struct, and that slot has to be releasable too -- see
 * fdtable_close_active().  @pid is used only for tracing and to scope the
 * per-process file locks; the boot table passes 0.
 */
int fdtable_close_files(files_struct_t *files, int fd, int pid)
{
    if (!files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    vfile_t *vf = files->fd[fd];
    if (!vf) {
        spin_unlock_irqrestore(&files->lock, flags);
        return -EBADF;
    }
    files->fd[fd] = NULL;
    files->cloexec[fd] = 0;
    fdtable_note_free(files, fd);
    spin_unlock_irqrestore(&files->lock, flags);
    /* Drop the envelope class registry entry for this slot before it can be
     * handed to a new descriptor.  env_kind_of() keys on the fd NUMBER, so a
     * byte left behind would classify the next occupant of this slot as the
     * descriptor that just left. */
    env_kind_unregister(fd);
    wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    ktrace_fd("[FD] close: pid=%d lfd=%d\n", pid, fd);
    vfs_release_process_file_locks(vf, pid);
    fdtable_slot_put(vf);
    return 0;
}

int fdtable_close_current(int fd)
{
    return fdtable_close(proc_current(), fd);
}

/* Release a slot in whichever table the caller is actually using.  With no
 * current task that is the boot table, which has no owning task, so
 * fdtable_close() cannot express it -- the slot used to survive the close,
 * leaving a permanently occupied slot and an unreleased vfile reference behind
 * for every descriptor opened before the first task existed. */
int fdtable_close_active(int fd)
{
    task_t *cur = proc_current();
    if (cur)
        return fdtable_close(cur, fd);
    return fdtable_close_files(fdtable_boot(), fd, 0);
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

    vfile_t *vf = files->fd[oldfd];
    if (!vf) {
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return -EBADF;
    }

    int fd = fdtable_find_free_below(files, minfd, limit);
    if (fd >= 0) {
        files->fd[fd] = vf;
        files->cloexec[fd] = (flags & O_CLOEXEC) ? 1 : 0;
        fdtable_note_alloc(files, fd);
        vfile_get(vf);
        spin_unlock_irqrestore(&files->lock, lock_flags);
        wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
        return fd;
    }
    spin_unlock_irqrestore(&files->lock, lock_flags);
    return -EMFILE;
}

int fdtable_dup_current(int oldfd, int minfd, int flags)
{
    return fdtable_dup(proc_current(), oldfd, minfd, flags);
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
    vfile_t *vf = files->fd[oldfd];
    if (!vf) {
        spin_unlock_irqrestore(&files->lock, lock_flags);
        return -EBADF;
    }

    vfile_t *old_new_vf = files->fd[newfd];
    files->fd[newfd] = NULL;
    files->cloexec[newfd] = 0;
    fdtable_note_free(files, newfd);
    files->fd[newfd] = vf;
    files->cloexec[newfd] = (flags & O_CLOEXEC) ? 1 : 0;
    fdtable_note_alloc(files, newfd);
    vfile_get(vf);
    spin_unlock_irqrestore(&files->lock, lock_flags);
    wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    if (old_new_vf) {
        /* dup2 onto an occupied slot: the displaced descriptor's class entry
         * is stale the moment the slot changes hands. */
        env_kind_unregister(newfd);
        vfs_release_process_file_locks(old_new_vf, task->pid);
        fdtable_slot_put(old_new_vf);
    }
    return newfd;
}

int fdtable_get_cloexec(task_t *task, int fd)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int ret = files->fd[fd] ? (files->cloexec[fd] ? FD_CLOEXEC : 0) : -EBADF;
    spin_unlock_irqrestore(&files->lock, flags);
    return ret;
}

int fdtable_set_cloexec(task_t *task, int fd, int cloexec)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    if (!files->fd[fd]) {
        spin_unlock_irqrestore(&files->lock, flags);
        return -EBADF;
    }
    files->cloexec[fd] = cloexec ? 1 : 0;
    spin_unlock_irqrestore(&files->lock, flags);
    return 0;
}

size_t fdtable_open_fd_count(void)
{
    size_t count = 0;
    /* E2: tasklist_lock walks the global list and each task's ->files is
     * park_lock-owned; the table's own lock is taken nested inside
     * (tasklist_lock -> park_lock -> files->lock). */
    uint64_t flags = spin_lock_irqsave(&tasklist_lock);
    for (task_t *t = proc_first_task_locked(); t;
         t = proc_next_task_locked(t)) {
        uint64_t tf = spin_lock_irqsave(&t->park_lock);
        files_struct_t *files = (files_struct_t *)t->files;
        if (files) {
            uint64_t ff = spin_lock_irqsave(&files->lock);
            for (int word = 0; word < FDTABLE_WORDS; word++)
                count += (size_t)__builtin_popcountll(files->open_mask[word]);
            spin_unlock_irqrestore(&files->lock, ff);
        }
        spin_unlock_irqrestore(&t->park_lock, tf);
    }
    spin_unlock_irqrestore(&tasklist_lock, flags);
    for (int word = 0; word < FDTABLE_WORDS; word++)
        count += (size_t)__builtin_popcountll(fdtable_boot_files.open_mask[word]);
    return count;
}

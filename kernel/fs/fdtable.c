#include "proc/resource_ns.h"
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

static int fdtable_stdio_install_locked(files_struct_t *files, int fd,
                                        vfile_t *vf);
static files_struct_t *fdtable_pin_task_files(const task_t *task);
static void fdtable_files_put(files_struct_t *files);
static files_struct_t *fdtable_publish_private(task_t *task,
                                               files_struct_t *replacement);

/* fd slots reference vfiles directly.  Closing a slot drops that reference;
 * when it is the last one the finalizer (ops->close + vfile_free +
 * vnode_put) must run.  The finalizer lives in vfs.c; fdtable.c is the only
 * caller outside vfs.c's own close paths. */
int vfs_finalize_closed_vfile(vfile_t *vf);
/* Implemented by eventpoll.c. Called only with a detached slot reference
 * alive and after every fdtable/task-publication lock has been released. */
int eventpoll_file_has_watch(vfile_t *vf);
void eventpoll_file_final_slot_closed(vfile_t *vf);

int a20_fdtable_find_free_rust(const uint64_t *mask, size_t max_files,
                               int min_fd);
int a20_fdtable_mark_alloc_rust(uint64_t *mask, int *next_fd,
                                size_t max_files, int fd);
int a20_fdtable_mark_free_rust(uint64_t *mask, int *next_fd,
                               size_t max_files, int fd);
int a20_fdtable_install_rust(void **slots, uint8_t *cloexec, uint64_t *mask,
                             int *next_fd, size_t max_files, size_t limit,
                             void *vfile, bool set_cloexec);
int a20_fdtable_detach_rust(void **slots, uint8_t *cloexec, uint64_t *mask,
                            int *next_fd, size_t max_files, int fd,
                            void **detached);
int a20_fdtable_detach_if_rust(void **slots, uint8_t *cloexec,
                               uint64_t *mask, int *next_fd,
                               size_t max_files, int fd, void *expected_vfile,
                               void **detached);
int a20_fdtable_dup_rust(void **slots, uint8_t *cloexec, uint64_t *mask,
                         int *next_fd, size_t max_files, size_t limit,
                         int old_fd, int min_fd, bool set_cloexec,
                         void (*get_ref)(void *));
int a20_fdtable_dup_to_rust(void **slots, uint8_t *cloexec, uint64_t *mask,
                            int *next_fd, size_t max_files, size_t limit,
                            int old_fd, int new_fd, bool set_cloexec,
                            void (*get_ref)(void *), void **displaced);
int a20_fdtable_clone_rust(void **src_slots, uint8_t *src_cloexec,
                           uint64_t *src_mask, int src_next_fd,
                           void **dst_slots, uint8_t *dst_cloexec,
                           uint64_t *dst_mask, int *dst_next_fd,
                           size_t max_files, void (*get_ref)(void *));
int a20_fdtable_clone_checked_rust(
    void **src_slots, uint8_t *src_cloexec, uint64_t *src_mask,
    int src_next_fd, void **dst_slots, uint8_t *dst_cloexec,
    uint64_t *dst_mask, int *dst_next_fd, size_t max_files,
    int (*try_get_ref)(void *), void (*put_ref)(void *));
int a20_fdtable_stdio_install_rust(void **slots, uint8_t *cloexec,
                                   uint64_t *mask, int *next_fd,
                                   size_t max_files, size_t fd, void *vfile,
                                   void (*get_ref)(void *));
int a20_fdtable_owner_acquire_rust(int *owners, void *table,
                                   void (*get_table_ref)(void *));
int a20_fdtable_owner_release_rust(int *owners, int *release_owner_pid,
                                   int pid);
int a20_fdtable_owner_is_shared_rust(int owners);
int a20_fdtable_cloexec_rust(void **slots, uint8_t *cloexec,
                             size_t max_files, int fd, int write_value);
int a20_fdtable_validate_rust(void **slots, uint8_t *cloexec,
                              uint64_t *mask, size_t max_files);
int a20_fdtable_detach_bulk_rust(void **slots, uint8_t *cloexec,
                                 uint64_t *mask, int *next_fd,
                                 size_t max_files, bool cloexec_only,
                                 void **out_vfiles, size_t out_capacity,
                                 void (*on_detach)(int));
int a20_fdtable_detach_next_rust(void **slots, uint8_t *cloexec,
                                 uint64_t *mask, int *next_fd,
                                 size_t max_files, bool cloexec_only,
                                 size_t start_fd, int *out_fd,
                                 void **out_vfile,
                                 void (*on_detach)(int));
int a20_fdtable_fd_limit(int has_task, uint64_t nofile, int max_files);
bool a20_fdtable_has_open_slot_rust(void *const *slots,
                                    const uint64_t *mask,
                                    size_t max_files, void *vfile);

/* Pinned for the life of the kernel: fd operations with no current task
 * resolve here.  It never takes part in the refcount and is never torn
 * down, so fdtable_files_put() treats it specially. */
static files_struct_t fdtable_boot_files;
static bool fdtable_boot_stdio_done;
static wait_queue_t fdtable_global_close_waiters = WAIT_QUEUE_INIT;
/* Lock order for retained-fd scans is eventpoll graph -> registry -> one
 * files_struct.lock. Mutators never take this lock while holding a table
 * lock; unregister happens only after the table has been detached/emptied. */
static spinlock_t fdtable_registry_lock = SPINLOCK_INIT;
static files_struct_t *fdtable_registry;

static void fdtable_registry_add(files_struct_t *files)
{
    uint64_t flags = spin_lock_irqsave(&fdtable_registry_lock);
    if (!files->registry_registered) {
        files->registry_next = fdtable_registry;
        fdtable_registry = files;
        files->registry_registered = true;
    }
    spin_unlock_irqrestore(&fdtable_registry_lock, flags);
}

static void fdtable_registry_remove(files_struct_t *files)
{
    uint64_t flags = spin_lock_irqsave(&fdtable_registry_lock);
    if (files->registry_registered) {
        files_struct_t **link = &fdtable_registry;
        while (*link && *link != files)
            link = &(*link)->registry_next;
        if (*link == files)
            *link = files->registry_next;
        files->registry_next = NULL;
        files->registry_registered = false;
    }
    spin_unlock_irqrestore(&fdtable_registry_lock, flags);
}

static void fdtable_wake_global_close(void)
{
    wait_queue_wake_all(&fdtable_global_close_waiters, 0, PROC_WAKE_EVENT);
}

static bool fdtable_global_close_waiting(void)
{
    uint64_t flags = spin_lock_irqsave(&fdtable_global_close_waiters.lock);
    bool waiting = fdtable_global_close_waiters.head != NULL;
    spin_unlock_irqrestore(&fdtable_global_close_waiters.lock, flags);
    return waiting;
}

static void fdtable_wake_if_last_slot(vfile_t *vf)
{
    if (!vf || !eventpoll_file_has_watch(vf) ||
        fdtable_file_has_open_slot(vf))
        return;
    eventpoll_file_final_slot_closed(vf);
    if (fdtable_global_close_waiting())
        fdtable_wake_global_close();
}

static files_struct_t *fdtable_try_alloc_files(void)
{
    files_struct_t *files = kmalloc(sizeof(*files));
    if (!files)
        return NULL;
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
    files->registry_next = NULL;
    files->registry_registered = false;
    fdtable_registry_add(files);
    ktrace_fd("[FDDBG] files=%p lock=%p\n", (void *)files, (void *)&files->lock);
    return files;
}

static files_struct_t *fdtable_alloc_files(void)
{
    files_struct_t *files = fdtable_try_alloc_files();
    if (!files)
        panic("fdtable: no memory");
    return files;
}

/* Table that fd operations apply to when the caller has no current task:
 * the boot table, initialised on first use with stdio attached. */
static files_struct_t *fdtable_boot(void)
{
    fdtable_registry_add(&fdtable_boot_files);
    if (!fdtable_boot_stdio_done) {
        spin_lock(&fdtable_boot_files.lock);
        if (!fdtable_boot_stdio_done) {
            files_struct_t *files = &fdtable_boot_files;
            for (int fd = 0; fd < 3 && fd < MAX_FILES; fd++) {
                vfile_t *s = devfs_create_stdio(fd);
                if (!s)
                    break;
                int result = fdtable_stdio_install_locked(files, fd, s);
                if (result < 0)
                    panic("fdtable_boot: stdio slot %d inconsistent (%d)",
                          fd, result);
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



/* Drop one fd-slot reference and run the finalizer when it was the last. */
static int fdtable_slot_put(vfile_t *vf)
{
    if (!vf)
        return 0;
    if (vfile_put_ref_only(vf))
        return vfs_finalize_closed_vfile(vf);
    return 0;
}

static int fdtable_try_get_ref(vfile_t *vf)
{
#ifdef FDTABLE_HOST_TEST
    extern int fdtable_host_try_vfile_get(vfile_t *);
    return fdtable_host_try_vfile_get(vf);
#else
    vfile_get(vf);
    return 0;
#endif
}

static void fdtable_rust_get_ref(void *opaque)
{
    vfile_get((vfile_t *)opaque);
}

static int fdtable_rust_try_get_ref(void *opaque)
{
    return fdtable_try_get_ref((vfile_t *)opaque);
}

static void fdtable_rust_put_ref(void *opaque)
{
    if (vfile_put_ref_only((vfile_t *)opaque))
        panic("fdtable clone rollback lost source slot pin");
}

static void fdtable_rust_unregister_fd(int fd)
{
    env_kind_unregister(fd);
}

static void fdtable_rust_get_table_ref(void *opaque)
{
    refcount_inc(&((files_struct_t *)opaque)->refcount);
}

/* Caller holds files->lock. Rust owns owner-count transitions while C keeps
 * the atomic refcount primitive and releases table refs after unlocking. */
static int fdtable_owner_acquire_locked(files_struct_t *files)
{
    return a20_fdtable_owner_acquire_rust(
        &files->owners, files, fdtable_rust_get_table_ref);
}

/* Caller holds files->lock. The caller drops the table ref after unlocking. */
static int fdtable_owner_release_locked(files_struct_t *files, int pid)
{
    return a20_fdtable_owner_release_rust(&files->owners,
                                          &files->release_owner_pid, pid);
}

static bool fdtable_owner_is_shared_locked(files_struct_t *files)
{
    return a20_fdtable_owner_is_shared_rust(files->owners) != 0;
}


/* Called with files->lock held. A full validation pass always precedes these
 * bulk transitions, so the C cursor cannot partially mutate a malformed
 * table before discovering corruption later in the scan. */
static int fdtable_validate_locked(files_struct_t *files)
{
    return a20_fdtable_validate_rust((void **)files->fd, files->cloexec,
                                     files->open_mask, MAX_FILES);
}

/* Source table lock is held and destination is private/unpublished. This
 * preflights all source metadata, then acquires refs transactionally. The
 * production vfile reference increment cannot fail; the host hook injects a
 * failure at each boundary to prove exact rollback. */
static int fdtable_clone_locked(files_struct_t *src, files_struct_t *dst)
{
    /* Every production clone destination comes from fdtable_try_alloc_files()
     * and starts with the empty-table search hint. */
    if (dst->next_fd != 0)
        return -EINVAL;
    return a20_fdtable_clone_checked_rust(
        (void **)src->fd, src->cloexec, src->open_mask, src->next_fd,
        (void **)dst->fd, dst->cloexec, dst->open_mask, &dst->next_fd,
        MAX_FILES, fdtable_rust_try_get_ref, fdtable_rust_put_ref);
}

static int fdtable_stdio_install_locked(files_struct_t *files, int fd,
                                        vfile_t *vf)
{
    return a20_fdtable_stdio_install_rust(
        (void **)files->fd, files->cloexec, files->open_mask,
        &files->next_fd, MAX_FILES, (size_t)fd, vf, fdtable_rust_get_ref);
}

static int fdtable_detach_next_locked(files_struct_t *files,
                                      bool cloexec_only, size_t start_fd,
                                      int *out_fd, vfile_t **out_vfile)
{
    return a20_fdtable_detach_next_rust(
        (void **)files->fd, files->cloexec, files->open_mask,
        &files->next_fd, MAX_FILES, cloexec_only, start_fd, out_fd,
        (void **)out_vfile, fdtable_rust_unregister_fd);
}

static int fdtable_detach_bulk_locked(files_struct_t *files,
                                      bool cloexec_only, vfile_t **out,
                                      size_t capacity)
{
    return a20_fdtable_detach_bulk_rust(
        (void **)files->fd, files->cloexec, files->open_mask,
        &files->next_fd, MAX_FILES, cloexec_only, (void **)out, capacity,
        fdtable_rust_unregister_fd);
}

static void fdtable_files_put(files_struct_t *files)
{
    if (!files || !refcount_dec_and_test(&files->refcount))
        return;

    /* On the heap, not the stack: at MAX_FILES this array is 32 KiB and the
     * kernel stack is 64 KiB. */
    vfile_t **to_close = kmalloc(sizeof(vfile_t *) * MAX_FILES);
    if (!to_close) {
        /* Rust/C selection remains slot-at-a-time without heap scratch. Drop
         * the lock between each detach and callback so eventpoll may safely
         * inspect all live tables while the detached slot still pins its file. */
        int cursor = 0;
        bool validated = false;
        for (;;) {
            uint64_t f = spin_lock_irqsave(&files->lock);
            if (!validated) {
                int valid = fdtable_validate_locked(files);
                if (valid < 0) {
                    spin_unlock_irqrestore(&files->lock, f);
                    panic("fdtable_files_put: inconsistent descriptor table (%d)",
                          valid);
                }
                validated = true;
            }
            int fd = -1;
            vfile_t *vf = NULL;
            int result = fdtable_detach_next_locked(files, false,
                                                     (size_t)cursor,
                                                     &fd, &vf);
            if (result < 0) {
                spin_unlock_irqrestore(&files->lock, f);
                panic("fdtable_files_put: detach failed (%d)", result);
            }
            spin_unlock_irqrestore(&files->lock, f);
            if (result == 0)
                break;
            cursor = fd + 1;
            if (files == &fdtable_boot_files)
                continue;
            fdtable_wake_if_last_slot(vf);
            if (files->release_owner_pid >= 0)
                vfs_release_process_file_locks(vf, files->release_owner_pid);
            fdtable_slot_put(vf);
        }
        if (files != &fdtable_boot_files) {
            fdtable_registry_remove(files);
            kfree(files);
        }
        return;
    }
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int close_count = fdtable_detach_bulk_locked(files, false, to_close,
                                                  MAX_FILES);
    if (close_count < 0) {
        spin_unlock_irqrestore(&files->lock, flags);
        kfree(to_close);
        panic("fdtable_files_put: bulk detach failed (%d)", close_count);
    }
    spin_unlock_irqrestore(&files->lock, flags);

    if (files != &fdtable_boot_files) {
        for (int i = 0; i < close_count; i++) {
            fdtable_wake_if_last_slot(to_close[i]);
            if (files->release_owner_pid >= 0)
                vfs_release_process_file_locks(to_close[i],
                                               files->release_owner_pid);
            fdtable_slot_put(to_close[i]);
        }
        fdtable_registry_remove(files);
        kfree(files);
    }
    kfree(to_close);
}


static int fdtable_fd_limit(task_t *task)
{
    return a20_fdtable_fd_limit(task ? 1 : 0, task ? task->limits.nofile : 0,
                                MAX_FILES);
}




void fdtable_init(task_t *task)
{
    if (!task)
        return;
    files_struct_t *replacement = fdtable_alloc_files();
    files_struct_t *old = fdtable_publish_private(task, replacement);
    if (old)
        fdtable_files_put(old);
    fdtable_init_stdio(task);
}

void fdtable_init_stdio(task_t *task)
{
    files_struct_t *files = fdtable_files(task);
    if (!files)
        return;
    for (int fd = 0; fd < 3 && fd < MAX_FILES; fd++) {
        vfile_t *s = devfs_create_stdio(fd);
        if (!s)
            continue;
        uint64_t flags = spin_lock_irqsave(&files->lock);
        int result = fdtable_stdio_install_locked(files, fd, s);
        spin_unlock_irqrestore(&files->lock, flags);
        if (result < 0)
            panic("fdtable_init_stdio: slot %d inconsistent (%d)", fd,
                  result);
    }
}

static files_struct_t *fdtable_pin_task_files(const task_t *task)
{
    if (!task)
        return NULL;
    task_t *mutable_task = (task_t *)task;
    uint64_t flags = spin_lock_irqsave(&mutable_task->park_lock);
    files_struct_t *files = (files_struct_t *)task->files;
    if (files)
        refcount_inc(&files->refcount);
    spin_unlock_irqrestore(&mutable_task->park_lock, flags);
    return files;
}

/* Acquire one files_struct task-owner reference while its task pointer is
 * stable. The returned reference is intended to be transferred to another
 * task's ->files pointer; NULL with result 0 means the source has no table. */
static files_struct_t *fdtable_acquire_task_files_owner(const task_t *task,
                                                        int *result)
{
    if (result)
        *result = 0;
    if (!task)
        return NULL;
    task_t *mutable_task = (task_t *)task;
    uint64_t task_flags = spin_lock_irqsave(&mutable_task->park_lock);
    files_struct_t *files = (files_struct_t *)task->files;
    if (files) {
        uint64_t files_flags = spin_lock_irqsave(&files->lock);
        int acquire_result = fdtable_owner_acquire_locked(files);
        spin_unlock_irqrestore(&files->lock, files_flags);
        if (acquire_result < 0) {
            if (result)
                *result = acquire_result;
            files = NULL;
        }
    }
    spin_unlock_irqrestore(&mutable_task->park_lock, task_flags);
    return files;
}

/* Publish a completed private table. The task park lock protects ->files;
 * owner metadata remains protected by the old table lock. The returned old
 * owner reference must be released after both locks are dropped. */
/* Caller holds task->park_lock; old table owner release nests its table lock. */
static files_struct_t *fdtable_publish_private_locked(
    task_t *task, files_struct_t *replacement)
{
    files_struct_t *old = (files_struct_t *)task->files;
    task->files = replacement;
    if (old) {
        uint64_t old_flags = spin_lock_irqsave(&old->lock);
        int remaining = fdtable_owner_release_locked(old, task->pid);
        if (remaining < 0) {
            spin_unlock_irqrestore(&old->lock, old_flags);
            panic("fdtable publish: invalid old owner count");
        }
        spin_unlock_irqrestore(&old->lock, old_flags);
    }
    return old;
}

static files_struct_t *fdtable_publish_private(task_t *task,
                                              files_struct_t *replacement)
{
    uint64_t flags = spin_lock_irqsave(&task->park_lock);
    files_struct_t *old = fdtable_publish_private_locked(task, replacement);
    spin_unlock_irqrestore(&task->park_lock, flags);
    return old;
}

int fdtable_copy_checked(task_t *dst, const task_t *src)
{
    if (!dst)
        return -EINVAL;
    if (!src)
        return -ESRCH;
    files_struct_t *src_files = fdtable_pin_task_files(src);
    if (!src_files)
        return -ESRCH;
    files_struct_t *dst_files = fdtable_try_alloc_files();
    if (!dst_files) {
        fdtable_files_put(src_files);
        return -ENOMEM;
    }

    /* The private table is already registered, including before it is
     * published in dst->files. Hold its lock while cloning so a retained-file
     * scan sees either the empty table or the complete inherited slot set.
     * dst->park_lock separately serializes replacement of the task pointer;
     * no tasklist lock participates in this operation. */
    uint64_t dst_flags = spin_lock_irqsave(&dst->park_lock);
    uint64_t flags = spin_lock_irqsave(&src_files->lock);
    uint64_t dst_file_flags = spin_lock_irqsave(&dst_files->lock);
    int result = fdtable_clone_locked(src_files, dst_files);
    spin_unlock_irqrestore(&dst_files->lock, dst_file_flags);
    spin_unlock_irqrestore(&src_files->lock, flags);
#ifdef FDTABLE_HOST_TEST
    extern void fdtable_host_after_clone_before_publish(task_t *,
                                                        const task_t *,
                                                        files_struct_t *);
    if (result == 0)
        fdtable_host_after_clone_before_publish(dst, src, dst_files);
#endif
    if (result < 0) {
        spin_unlock_irqrestore(&dst->park_lock, dst_flags);
        fdtable_files_put(dst_files);
        fdtable_files_put(src_files);
        return result;
    }

    files_struct_t *old_dst = fdtable_publish_private_locked(dst, dst_files);
    spin_unlock_irqrestore(&dst->park_lock, dst_flags);
    if (old_dst)
        fdtable_files_put(old_dst);
    fdtable_files_put(src_files);
    return 0;
}

void fdtable_copy(task_t *dst, const task_t *src)
{
    if (!dst)
        return;
    if (!src) {
        files_struct_t *files = fdtable_try_alloc_files();
        if (!files)
            panic("fdtable_copy: no memory");
        files_struct_t *old = fdtable_publish_private(dst, files);
        if (old)
            fdtable_files_put(old);
        fdtable_init_stdio(dst);
        return;
    }
    int result = fdtable_copy_checked(dst, src);
    if (result == -ESRCH) {
        /* Preserve the legacy empty-source behavior without reading
         * src->files outside src->park_lock. */
        files_struct_t *files = fdtable_try_alloc_files();
        if (!files)
            panic("fdtable_copy: no memory");
        files_struct_t *old = fdtable_publish_private(dst, files);
        if (old)
            fdtable_files_put(old);
        fdtable_init_stdio(dst);
        return;
    }
    if (result < 0)
        panic("fdtable_copy: checked copy failed (%d)", result);
}

void fdtable_share(task_t *dst, const task_t *src)
{
    if (!dst || !src || dst == src)
        return;
    int acquire_result = 0;
    files_struct_t *source = fdtable_acquire_task_files_owner(src,
                                                              &acquire_result);
    if (acquire_result < 0)
        return;
#ifdef FDTABLE_HOST_TEST
    extern void fdtable_host_after_share_acquire(task_t *, files_struct_t *);
    fdtable_host_after_share_acquire((task_t *)src, source);
#endif
    /* The source task owner is acquired before releasing dst's previous
     * owner. This keeps the table alive if src tears down between acquisition
     * and publication, and avoids an empty-table window in dst. */
    uint64_t flags = spin_lock_irqsave(&dst->park_lock);
    files_struct_t *old = (files_struct_t *)dst->files;
    dst->files = source;
    if (old) {
        uint64_t old_flags = spin_lock_irqsave(&old->lock);
        int remaining = fdtable_owner_release_locked(old, dst->pid);
        spin_unlock_irqrestore(&old->lock, old_flags);
        if (remaining < 0) {
            spin_unlock_irqrestore(&dst->park_lock, flags);
            panic("fdtable_share: invalid destination owner count");
        }
    }
    spin_unlock_irqrestore(&dst->park_lock, flags);
    if (old)
        fdtable_files_put(old);
}

int fdtable_unshare(task_t *task)
{
    if (!task)
        return -ESRCH;
    files_struct_t *old = fdtable_pin_task_files(task);
    if (!old)
        return -EBADF;
    uint64_t owner_flags = spin_lock_irqsave(&old->lock);
    bool shared = fdtable_owner_is_shared_locked(old);
    spin_unlock_irqrestore(&old->lock, owner_flags);
    if (!shared) {
        fdtable_files_put(old);
        return 0;
    }

    files_struct_t *files = fdtable_try_alloc_files();
    if (!files) {
        fdtable_files_put(old);
        return -ENOMEM;
    }
    uint64_t flags = spin_lock_irqsave(&old->lock);
    uint64_t new_flags = spin_lock_irqsave(&files->lock);
    int clone_result = fdtable_clone_locked(old, files);
    spin_unlock_irqrestore(&files->lock, new_flags);
    spin_unlock_irqrestore(&old->lock, flags);
    if (clone_result < 0) {
        fdtable_files_put(files);
        fdtable_files_put(old);
        return clone_result;
    }
    /* The swap is the task's own park_lock critical section; the owner count
     * on the retired table stays under that table's lock. */
    uint64_t task_flags = spin_lock_irqsave(&task->park_lock);
    if (task->files != old) {
        spin_unlock_irqrestore(&task->park_lock, task_flags);
        fdtable_files_put(files);
        fdtable_files_put(old);
        return -EAGAIN;
    }
    uint64_t old_flags = spin_lock_irqsave(&old->lock);
    int remaining = fdtable_owner_release_locked(old, task->pid);
    if (remaining < 0) {
        spin_unlock_irqrestore(&old->lock, old_flags);
        spin_unlock_irqrestore(&task->park_lock, task_flags);
        fdtable_files_put(files);
        fdtable_files_put(old);
        return -EAGAIN;
    }
    task->files = files;
    spin_unlock_irqrestore(&old->lock, old_flags);
    spin_unlock_irqrestore(&task->park_lock, task_flags);
    /* Drop the replaced task-owner reference and then our source pin. */
    fdtable_files_put(old);
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
        uint64_t files_flags = spin_lock_irqsave(&files->lock);
        int remaining = fdtable_owner_release_locked(files, task->pid);
        spin_unlock_irqrestore(&files->lock, files_flags);
        if (remaining < 0) {
            spin_unlock_irqrestore(&task->park_lock, flags);
            panic("fdtable_release_files: invalid owner count");
        }
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
    resource_ns_release_task(task);
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
    /* Heap, not the stack: MAX_FILES pointers is 32 KiB against a 64 KiB
     * kernel stack.  A descriptor carrying O_CLOEXEC is normally a handful, so
     * on allocation failure fall back to dropping them in place. */
    vfile_t **to_close = kmalloc(sizeof(vfile_t *) * MAX_FILES);
    if (!to_close) {
        int valid = fdtable_validate_locked(files);
        if (valid < 0) {
            spin_unlock_irqrestore(&files->lock, flags);
            panic("fdtable_close_on_exec: inconsistent descriptor table (%d)",
                  valid);
        }
        int cursor = 0;
        bool closed_any = false;
        for (;;) {
            int fd = -1;
            vfile_t *vf = NULL;
            int result = fdtable_detach_next_locked(files, true,
                                                     (size_t)cursor,
                                                     &fd, &vf);
            if (result < 0) {
                spin_unlock_irqrestore(&files->lock, flags);
                panic("fdtable_close_on_exec: detach failed (%d)", result);
            }
            if (result == 0) {
                spin_unlock_irqrestore(&files->lock, flags);
                break;
            }
            cursor = fd + 1;
            spin_unlock_irqrestore(&files->lock, flags);
            /* OOM cursor cleanup keeps only this detached reference on the
             * stack; notify eventpoll and finalize outside files_struct.lock. */
            wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
            fdtable_wake_if_last_slot(vf);
            vfs_release_process_file_locks(vf, task->pid);
            fdtable_slot_put(vf);
            closed_any = true;
            flags = spin_lock_irqsave(&files->lock);
        }
        if (closed_any) {
            wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
        }
        fdtable_init_stdio(task);
        return;
    }
    int close_count = fdtable_detach_bulk_locked(files, true, to_close,
                                                  MAX_FILES);
    if (close_count < 0) {
        spin_unlock_irqrestore(&files->lock, flags);
        kfree(to_close);
        panic("fdtable_close_on_exec: bulk detach failed (%d)", close_count);
    }
    spin_unlock_irqrestore(&files->lock, flags);
    if (close_count) {
        wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    }
    for (int i = 0; i < close_count; i++) {
        fdtable_wake_if_last_slot(to_close[i]);
        vfs_release_process_file_locks(to_close[i], task->pid);
        fdtable_slot_put(to_close[i]);
    }
    kfree(to_close);
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

wait_queue_t *fdtable_global_close_readiness_queue(void)
{
    return &fdtable_global_close_waiters;
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

int fdtable_with_current_vfile(int local_fd, vfile_t *expected_vf,
                               int (*callback)(vfile_t *, void *),
                               void *context)
{
    if (local_fd < 0 || local_fd >= MAX_FILES || !expected_vf || !callback)
        return -EBADF;

    task_t *task = proc_current();
    files_struct_t *files;
    bool files_pinned = false;
    if (task) {
        /* Pin the table before dropping park_lock so concurrent exec/unshare
         * cannot retire its storage while this operation waits for files.lock. */
        uint64_t task_flags = spin_lock_irqsave(&task->park_lock);
        files = (files_struct_t *)task->files;
        if (files && !refcount_inc_not_zero(&files->refcount))
            files = NULL;
        files_pinned = files != NULL;
        spin_unlock_irqrestore(&task->park_lock, task_flags);
    } else {
        /* The boot table is pinned for kernel lifetime. Initialization occurs
         * only on this no-task operation path, never in a readiness query. */
        files = fdtable_boot();
    }
    if (!files)
        return -EBADF;

    /* Lock order: task->park_lock (released above), then files->lock, then the
     * callback may take envelope.lock. No caller may acquire files->lock while
     * holding envelope.lock. The callback must not allocate, sleep, copy user
     * data, signal tasks, or invoke VFS. expected_vf is caller-pinned. */
    uint64_t flags = spin_lock_irqsave(&files->lock);
    vfile_t *vf = files->fd[local_fd];
    int result = (!vf || vf != expected_vf || vf->identity != expected_vf->identity)
                     ? -EBADF
                     : callback(vf, context);
    spin_unlock_irqrestore(&files->lock, flags);
    if (files_pinned)
        fdtable_files_put(files);
    return result;
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
    int fd = a20_fdtable_install_rust((void **)files->fd, files->cloexec,
                                      files->open_mask, &files->next_fd,
                                      MAX_FILES, (size_t)limit, vf,
                                      (flags & O_CLOEXEC) != 0);
    if (fd >= 0) {
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
    return fdtable_close_files_if_file(files, fd, pid, NULL);
}

int fdtable_close_files_if_file(files_struct_t *files, int fd, int pid,
                                vfile_t *expected_vf)
{
    if (!files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    vfile_t *vf = NULL;
    int detach_result = expected_vf ?
        a20_fdtable_detach_if_rust((void **)files->fd, files->cloexec,
                                   files->open_mask, &files->next_fd,
                                   MAX_FILES, fd, expected_vf, (void **)&vf) :
        a20_fdtable_detach_rust((void **)files->fd, files->cloexec,
                                files->open_mask, &files->next_fd,
                                MAX_FILES, fd, (void **)&vf);
    if (detach_result < 0) {
        spin_unlock_irqrestore(&files->lock, flags);
        return detach_result;
    }
    spin_unlock_irqrestore(&files->lock, flags);
    /* Drop the envelope class registry entry for this slot before it can be
     * handed to a new descriptor.  env_kind_of() keys on the fd NUMBER, so a
     * byte left behind would classify the next occupant of this slot as the
     * descriptor that just left. */
    env_kind_unregister(fd);
    wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    fdtable_wake_if_last_slot(vf);
    ktrace_fd("[FD] close: pid=%d lfd=%d\n", pid, fd);
    vfs_release_process_file_locks(vf, pid);
    /* The slot is already detached and locks are released. Report final-close
     * errors without restoring or touching a possible replacement slot. */
    return fdtable_slot_put(vf);
}

int fdtable_close_current(int fd)
{
    return fdtable_close(proc_current(), fd);
}

int fdtable_close_if_file(task_t *task, int fd, vfile_t *expected_vf)
{
    if (!task || !task->files)
        return -EBADF;
    return fdtable_close_files_if_file((files_struct_t *)task->files, fd,
                                       task->pid, expected_vf);
}

int fdtable_close_current_if_file(int fd, vfile_t *expected_vf)
{
    return fdtable_close_if_file(proc_current(), fd, expected_vf);
}

#ifdef CONFIG_ENVELOPE_ACQUIRE_RACE_TEST
int fdtable_test_replace_current_if_file(int fd, vfile_t *expected_vf,
                                         vfile_t *replacement_vf)
{
    task_t *task = proc_current();
    if (!task || !expected_vf || !replacement_vf || fd < 0 ||
        fd >= MAX_FILES)
        return -EINVAL;
    files_struct_t *files = fdtable_active_files(task);
    if (!files)
        return -EBADF;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    if (files->fd[fd] != expected_vf ||
        files->fd[fd]->identity != expected_vf->identity) {
        spin_unlock_irqrestore(&files->lock, flags);
        return -EBADF;
    }
    uint8_t cloexec = files->cloexec[fd];
    vfile_get(replacement_vf);
    files->fd[fd] = replacement_vf;
    files->cloexec[fd] = cloexec;
    spin_unlock_irqrestore(&files->lock, flags);

    env_kind_unregister(fd);
    wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    fdtable_wake_if_last_slot(expected_vf);
    vfs_release_process_file_locks(expected_vf, task->pid);
    return fdtable_slot_put(expected_vf);
}
#endif

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
    int fd = a20_fdtable_dup_rust((void **)files->fd, files->cloexec,
                                  files->open_mask, &files->next_fd,
                                  MAX_FILES, (size_t)limit, oldfd, minfd,
                                  (flags & O_CLOEXEC) != 0,
                                  fdtable_rust_get_ref);
    spin_unlock_irqrestore(&files->lock, lock_flags);
    if (fd >= 0)
        wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    return fd;
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
    vfile_t *old_new_vf = NULL;
    int rust_result = a20_fdtable_dup_to_rust(
        (void **)files->fd, files->cloexec, files->open_mask,
        &files->next_fd, MAX_FILES, (size_t)fdtable_fd_limit(task),
        oldfd, newfd, (flags & O_CLOEXEC) != 0, fdtable_rust_get_ref,
        (void **)&old_new_vf);
    spin_unlock_irqrestore(&files->lock, lock_flags);
    if (rust_result < 0)
        return rust_result;
    wait_queue_wake_all(&files->readiness_waiters, 0, PROC_WAKE_EVENT);
    if (old_new_vf) {
        env_kind_unregister(newfd);
        fdtable_wake_if_last_slot(old_new_vf);
        vfs_release_process_file_locks(old_new_vf, task->pid);
        fdtable_slot_put(old_new_vf);
    }
    return rust_result;
}

int fdtable_get_cloexec(task_t *task, int fd)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int ret = a20_fdtable_cloexec_rust((void **)files->fd, files->cloexec,
                                       MAX_FILES, fd, -1);
    spin_unlock_irqrestore(&files->lock, flags);
    return ret;
}

int fdtable_set_cloexec(task_t *task, int fd, int cloexec)
{
    if (!task || !task->files || fd < 0 || fd >= MAX_FILES)
        return -EBADF;
    files_struct_t *files = (files_struct_t *)task->files;
    uint64_t flags = spin_lock_irqsave(&files->lock);
    int ret = a20_fdtable_cloexec_rust((void **)files->fd, files->cloexec,
                                       MAX_FILES, fd, cloexec != 0);
    spin_unlock_irqrestore(&files->lock, flags);
    return ret;
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

bool fdtable_file_has_open_slot(vfile_t *vf)
{
    if (!vf)
        return false;
    bool found = false;
    /* Unlike tasklist iteration this includes unpublished fork destinations:
     * their slot refs become scan-visible immediately as clone fills the
     * registered private table. Lock order is eventpoll graph -> registry ->
     * table. No path acquires the registry while holding a table lock. */
    uint64_t flags = spin_lock_irqsave(&fdtable_registry_lock);
    for (files_struct_t *files = fdtable_registry; files && !found;
         files = files->registry_next) {
        uint64_t ff = spin_lock_irqsave(&files->lock);
        found = a20_fdtable_has_open_slot_rust((void *const *)files->fd,
                                                files->open_mask, MAX_FILES,
                                                vf);
        spin_unlock_irqrestore(&files->lock, ff);
    }
    spin_unlock_irqrestore(&fdtable_registry_lock, flags);
    return found;
}

#include <errno.h>
#include <pthread.h>
#include <setjmp.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "fs/fdtable.h"
#include "fs/vfs.h"

spinlock_t tasklist_lock = SPINLOCK_INIT;
static task_t *current_task;
static int finalized_files;
static int finalizer_error;
static int fail_next_kmalloc;
static int fail_vfile_get_at = -1;
static int vfile_get_attempts;
static void *expected_table_free;
static int expected_table_free_count;
static jmp_buf panic_jmp;
static int panic_expected;
static pthread_mutex_t close_race_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t close_race_cond = PTHREAD_COND_INITIALIZER;
static vfile_t *close_race_vfile;
static task_t *close_race_task;
static vfile_t *close_race_replacement;
static int close_race_notified;
static int close_race_installed_fd = -1;
static int close_race_install_done;
static task_t *share_teardown_source;
static vfile_t stdio_vfiles[3];
static int stdio_stub_enabled;
static task_t *clone_race_source;
static vfile_t *clone_race_file;
static int clone_race_hook_calls;
static int final_slot_hook_calls;

static void check(bool ok, const char *what);

task_t *proc_current(void) { return current_task; }
task_t *proc_first_task_locked(void) { return current_task; }
task_t *proc_next_task_locked(task_t *task) { (void)task; return NULL; }
int proc_task_pid(const void *task) { return task ? ((const task_t *)task)->pid : -1; }

void *kmalloc(size_t size)
{
    if (__atomic_exchange_n(&fail_next_kmalloc, 0, __ATOMIC_RELAXED))
        return NULL;
    return malloc(size);
}
void kfree(void *ptr)
{
    if (ptr && ptr == expected_table_free)
        expected_table_free_count++;
    free(ptr);
}
void panic(const char *fmt, ...)
{
    va_list args;
    va_start(args, fmt);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
    if (panic_expected) {
        panic_expected = 0;
        longjmp(panic_jmp, 1);
    }
    abort();
}
uint64_t timer_get_ticks(void) { return 0; }
unsigned x86_64_apic_to_cpu(unsigned apic_id) { return apic_id; }
void kallsyms_print(uint64_t addr) { (void)addr; }
vfile_t *devfs_create_stdio(int fd)
{
    return stdio_stub_enabled && fd >= 0 && fd < 3 ? &stdio_vfiles[fd] : NULL;
}
void env_kind_unregister(int fd) { (void)fd; }
int eventpoll_file_has_watch(vfile_t *vf) { return vf != NULL; }
void eventpoll_file_final_slot_closed(vfile_t *vf)
{
    /* Re-enter the global slot scan to assert the hook is never called with
     * tasklist/park/files locks held. */
    (void)fdtable_file_has_open_slot(vf);
    final_slot_hook_calls++;
    if (vf != close_race_vfile)
        return;
    pthread_mutex_lock(&close_race_lock);
    close_race_notified = 1;
    pthread_cond_broadcast(&close_race_cond);
    while (!close_race_install_done)
        pthread_cond_wait(&close_race_cond, &close_race_lock);
    pthread_mutex_unlock(&close_race_lock);
}
void fdtable_host_after_clone_before_publish(task_t *dst, const task_t *src,
                                              files_struct_t *private_files)
{
    if (src != clone_race_source)
        return;
    clone_race_hook_calls++;
    files_struct_t *source_files = (files_struct_t *)src->files;
    check(private_files && private_files != source_files &&
              private_files->fd[0] == clone_race_file,
          "clone hook sees populated private registered table");
    check(fdtable_close_files(source_files, 0, src->pid) == 0,
          "source last slot closes before child publication");
    check(fdtable_file_has_open_slot(clone_race_file),
          "registry scan finds unlisted private clone slot");
    clone_race_source = NULL;
}
void fdtable_host_after_share_acquire(task_t *src, files_struct_t *files)
{
    if (src == share_teardown_source) {
        check(files == src->files, "share hook sees pinned source table");
        share_teardown_source = NULL;
        /* Runs after source park/files locks are released, between acquiring
         * the new owner and publishing it in the destination task. */
        fdtable_release_files(src);
        check(files->fd[0] && fdtable_file_has_open_slot(files->fd[0]),
              "registry scan sees acquired share owner before publication");
    }
}
int env_active(task_t *task) { (void)task; return 0; }
void mntns_release_task(task_t *task) { (void)task; }
void pidns_release_task(task_t *task) { (void)task; }
void resource_ns_release_task(task_t *task) { (void)task; }
void userns_release_task(task_t *task) { (void)task; }
void vfs_task_fs_pins_release(task_t *task) { (void)task; }
void vfs_release_process_file_locks(vfile_t *vf, int pid) { (void)vf; (void)pid; }

void vfile_get(vfile_t *vf)
{
    __atomic_add_fetch(&vf->ref_count.value, 1, __ATOMIC_RELAXED);
}

int fdtable_host_try_vfile_get(vfile_t *vf)
{
    int attempt = vfile_get_attempts++;
    if (attempt == fail_vfile_get_at)
        return -ENOMEM;
    vfile_get(vf);
    return 0;
}

int vfile_put_ref_only(vfile_t *vf)
{
    return __atomic_sub_fetch(&vf->ref_count.value, 1, __ATOMIC_ACQ_REL) == 0;
}

void vfs_put_file(vfile_t *vf)
{
    if (vf && vfile_put_ref_only(vf))
        vfs_finalize_closed_vfile(vf);
}

int vfs_finalize_closed_vfile(vfile_t *vf)
{
    if (vf)
        __atomic_add_fetch(&finalized_files, 1, __ATOMIC_RELAXED);
    return finalizer_error;
}

static void check(bool ok, const char *what)
{
    if (!ok) {
        fprintf(stderr, "FDTABLE_DIFF: FAIL %s\n", what);
        exit(1);
    }
}

static void init_vfile(vfile_t *vf, uint64_t identity)
{
    vf->identity = identity;
    refcount_set(&vf->ref_count, 1);
}

static void init_task(task_t *task, int pid)
{
    memset(task, 0, sizeof(*task));
    task->pid = pid;
    task->limits.nofile = MAX_FILES;
    spin_init(&task->park_lock);
    fdtable_init(task);
    current_task = task;
    check(task->files != NULL, "fdtable_init");
}

typedef struct {
    vfile_t *seen;
    int calls;
    int result;
} vfile_callback_arg_t;

static int inspect_current_vfile(vfile_t *vf, void *opaque)
{
    vfile_callback_arg_t *arg = opaque;
    arg->seen = vf;
    arg->calls++;
    return arg->result;
}

static void close_all_slots(task_t *task)
{
    files_struct_t *files = (files_struct_t *)task->files;
    for (int fd = 0; fd < MAX_FILES; fd++)
        if (files->fd[fd])
            check(fdtable_close_files(files, fd, task->pid) == 0, "close slot");
}

static void test_lifecycle(void)
{
    task_t parent, child;
    vfile_t a, b, c, d;
    init_task(&parent, 41);
    current_task = &parent;
    init_vfile(&a, 1);
    init_vfile(&b, 2);
    init_vfile(&c, 3);
    init_vfile(&d, 4);

    int afd = fdtable_install_vfile(&parent, &a, 0);
    check(afd == 0, "install allocates lowest slot");
    check(fdtable_set_cloexec(&parent, afd, 1) == 0 &&
              fdtable_get_cloexec(&parent, afd) == FD_CLOEXEC,
          "cloexec set/get");

    parent.limits.nofile = 64;
    check(fdtable_dup(&parent, afd, 64, 0) == -EMFILE,
          "permission/rlimit boundary rejects dup");
    parent.limits.nofile = MAX_FILES;
    int adup = fdtable_dup(&parent, afd, 64, O_CLOEXEC);
    check(adup == 64, "bitmap word boundary allocation");
    check(__atomic_load_n(&a.ref_count.value, __ATOMIC_RELAXED) == 2,
          "dup acquires one file ref");
    check(fdtable_get_cloexec(&parent, adup) == FD_CLOEXEC,
          "dup cloexec flag");

    int bfd = fdtable_install_vfile(&parent, &b, 0);
    check(bfd == 65, "next_fd hint advances across bitmap words");
    check(fdtable_dup_to(&parent, bfd, adup, 0) == adup,
          "dup-to displaces target");
    check(((files_struct_t *)parent.files)->fd[adup] == &b,
          "dup-to publishes source identity");
    check(__atomic_load_n(&a.ref_count.value, __ATOMIC_RELAXED) == 1 &&
              __atomic_load_n(&b.ref_count.value, __ATOMIC_RELAXED) == 2,
          "dup-to balances acquired and displaced refs");
    check(fdtable_get_cloexec(&parent, adup) == 0,
          "dup-to replaces close-on-exec state");

    memset(&child, 0, sizeof(child));
    child.pid = 42;
    child.limits.nofile = MAX_FILES;
    spin_init(&child.park_lock);
    fdtable_copy(&child, &parent);
    files_struct_t *child_files = (files_struct_t *)child.files;
    check(child_files && child_files->fd[afd] == &a &&
              child_files->fd[bfd] == &b && child_files->fd[adup] == &b,
          "fork copy preserves slot identities");
    check(__atomic_load_n(&a.ref_count.value, __ATOMIC_RELAXED) == 2 &&
              __atomic_load_n(&b.ref_count.value, __ATOMIC_RELAXED) == 4,
          "fork copy acquires one ref per slot");

    fdtable_close_on_exec(&parent);
    files_struct_t *parent_files = (files_struct_t *)parent.files;
    check(parent_files->fd[afd] == NULL && parent_files->fd[bfd] == &b &&
              parent_files->fd[adup] == &b,
          "close-on-exec removes only flagged slot");
    check(__atomic_load_n(&a.ref_count.value, __ATOMIC_RELAXED) == 1,
          "close-on-exec drops one slot ref");

    int rollback_fd = fdtable_install_vfile(&parent, &c, 0);
    check(rollback_fd == afd, "closed descriptor reused");
    check(fdtable_close_current_if_file(rollback_fd, &c) == 0,
          "conditional close removes expected identity");
    int replacement_fd = fdtable_install_vfile(&parent, &d, 0);
    check(replacement_fd == rollback_fd, "replacement reuses same descriptor");
    check(fdtable_close_current_if_file(replacement_fd, &c) == -EBADF,
          "stale rollback refuses replacement");
    check(parent_files->fd[replacement_fd] == &d && d.ref_count.value == 1,
          "stale rollback preserves replacement and ref");

    vfile_callback_arg_t callback = { .result = 37 };
    check(fdtable_with_current_vfile(replacement_fd, &d,
                                     inspect_current_vfile, &callback) == 37,
          "identity-bound callback result");
    check(callback.calls == 1 && callback.seen == &d,
          "callback runs once for matching slot identity");
    check(fdtable_with_current_vfile(replacement_fd, &c,
                                     inspect_current_vfile, &callback) == -EBADF,
          "identity-bound callback rejects replaced slot");
    check(callback.calls == 1 && callback.seen == &d,
          "mismatched slot never invokes callback");

    task_t sibling;
    memset(&sibling, 0, sizeof(sibling));
    sibling.pid = 43;
    sibling.limits.nofile = MAX_FILES;
    spin_init(&sibling.park_lock);
    fdtable_share(&sibling, &parent);
    check(sibling.files == parent.files && parent_files->owners == 2,
          "CLONE_FILES shares one table with owner accounting");
    /* An occupied bitmap bit without a vfile makes clone validation fail.
     * unshare must discard its partially allocated destination and retain
     * the shared table and all existing file references. */
    const int malformed_fd = MAX_FILES - 1;
    parent_files->open_mask[malformed_fd / 64] |=
        UINT64_C(1) << (malformed_fd % 64);
    int old_table_refs = parent_files->refcount.value;
    int old_vfile_refs = b.ref_count.value;
    check(fdtable_unshare(&sibling) == -EBADF,
          "unshare rejects malformed occupied source slot");
    check(sibling.files == parent.files && parent_files->owners == 2 &&
              parent_files->refcount.value == old_table_refs &&
              b.ref_count.value == old_vfile_refs,
          "failed unshare rolls back destination and references");
    parent_files->open_mask[malformed_fd / 64] &=
        ~(UINT64_C(1) << (malformed_fd % 64));
    check(fdtable_unshare(&sibling) == 0,
          "valid shared table unshares successfully");
    files_struct_t *sibling_files = (files_struct_t *)sibling.files;
    check(sibling_files != parent_files && sibling_files->fd[bfd] == &b &&
              sibling_files->fd[adup] == &b && parent_files->owners == 1 &&
              b.ref_count.value == old_vfile_refs + 2,
          "successful unshare clones slots and acquires refs");
    check(fdtable_close_files(sibling_files, bfd, sibling.pid) == 0 &&
              parent_files->fd[bfd] == &b &&
              b.ref_count.value == old_vfile_refs + 1,
          "close after unshare affects only the private table");
    fdtable_release_files(&sibling);
    puts("FDTABLE_DIFF: lifecycle, permission, boundary, rollback PASS");

    close_all_slots(&child);
    close_all_slots(&parent);
    check(a.ref_count.value == 0 && b.ref_count.value == 0 &&
              c.ref_count.value == 0 && d.ref_count.value == 0,
          "all lifecycle refs released exactly once");
    fdtable_release_files(&child);
    fdtable_release_files(&parent);
}

static void test_checked_copy_failure_atomicity(void)
{
    task_t src, dst;
    vfile_t inherited[3], replaced;
    init_task(&src, 61);
    init_task(&dst, 62);
    current_task = &src;
    for (int i = 0; i < 3; i++)
        init_vfile(&inherited[i], (uint64_t)(610 + i));
    init_vfile(&replaced, 699);
    check(fdtable_install_vfile(&src, &inherited[0], 0) == 0,
          "copy source fd 0");
    ((files_struct_t *)src.files)->next_fd = 64;
    check(fdtable_install_vfile(&src, &inherited[1], 0) == 64,
          "copy source fd 64");
    ((files_struct_t *)src.files)->next_fd = 129;
    check(fdtable_install_vfile(&src, &inherited[2], 0) == 129,
          "copy source fd 129");
    check(fdtable_set_cloexec(&src, 64, 1) == 0,
          "copy source cloexec setup");
    current_task = &dst;
    check(fdtable_install_vfile(&dst, &replaced, 0) == 0,
          "existing destination slot");
    files_struct_t *original_dst = (files_struct_t *)dst.files;
    check(original_dst->owners == 1 && replaced.ref_count.value == 1,
          "existing destination baseline");

    fail_next_kmalloc = 1;
    check(fdtable_copy_checked(&dst, &src) == -ENOMEM,
          "checked copy table allocation failure");
    check(dst.files == original_dst && original_dst->owners == 1 &&
              original_dst->fd[0] == &replaced && replaced.ref_count.value == 1,
          "allocation failure preserves destination pointer/owners/refs");
    for (int failure = 0; failure < 3; failure++) {
        fail_vfile_get_at = failure;
        vfile_get_attempts = 0;
        check(fdtable_copy_checked(&dst, &src) == -ENOMEM,
              "checked copy injected vfile-ref failure");
        check(dst.files == original_dst && original_dst->owners == 1 &&
                  original_dst->fd[0] == &replaced &&
                  replaced.ref_count.value == 1,
              "ref failure preserves existing destination");
        for (int i = 0; i < 3; i++)
            check(inherited[i].ref_count.value == 1,
                  "ref failure rolls back every acquired prefix ref");
    }
    fail_vfile_get_at = -1;
    vfile_get_attempts = 0;
    check(fdtable_copy_checked(&dst, &src) == 0,
          "checked copy publishes complete table");
    files_struct_t *copy = (files_struct_t *)dst.files;
    check(copy != original_dst && copy->owners == 1 && copy->fd[0] == &inherited[0] &&
              copy->fd[64] == &inherited[1] && copy->fd[129] == &inherited[2] &&
              copy->cloexec[64] == 1,
          "published clone has all slots and metadata");
    check(replaced.ref_count.value == 0,
          "successful publish releases replaced destination table");
    for (int i = 0; i < 3; i++)
        check(inherited[i].ref_count.value == 2,
              "successful copy acquires one ref per open slot");

    files_struct_t *src_table = (files_struct_t *)src.files;
    src_table->open_mask[64 >> 6] &= ~(1ULL << (64 & 63));
    check(fdtable_copy_checked(&dst, &src) == -EBADF,
          "malformed reverse bitmap source rejected");
    check(dst.files == copy && copy->owners == 1 &&
              inherited[0].ref_count.value == 2 &&
              inherited[1].ref_count.value == 2 &&
              inherited[2].ref_count.value == 2,
          "malformed source leaves published destination unchanged");
    src_table->open_mask[64 >> 6] |= 1ULL << (64 & 63);

    close_all_slots(&src);
    close_all_slots(&dst);
    fdtable_release_files(&src);
    fdtable_release_files(&dst);
    puts("FDTABLE_DIFF: checked copy allocation/ref rollback and publish PASS");
}

static void test_unshare_allocation_failure_preserves_shared_table(void)
{
    task_t parent, child;
    vfile_t vf;
    init_task(&parent, 63);
    current_task = &parent;
    init_vfile(&vf, 630);
    check(fdtable_install_vfile(&parent, &vf, 0) == 0,
          "unshare source descriptor");
    memset(&child, 0, sizeof(child));
    child.pid = 64;
    child.limits.nofile = MAX_FILES;
    spin_init(&child.park_lock);
    fdtable_share(&child, &parent);
    files_struct_t *shared = (files_struct_t *)parent.files;
    check(child.files == shared && shared->owners == 2 &&
              __atomic_load_n(&shared->refcount.value, __ATOMIC_RELAXED) == 2 &&
              vf.ref_count.value == 1,
          "shared table owner/reference baseline");
    fail_next_kmalloc = 1;
    check(fdtable_unshare(&parent) == -ENOMEM,
          "unshare returns ENOMEM for private table allocation failure");
    check(parent.files == shared && child.files == shared && shared->owners == 2 &&
              __atomic_load_n(&shared->refcount.value, __ATOMIC_RELAXED) == 2 &&
              vf.ref_count.value == 1,
          "unshare allocation failure leaves old owners and file refs unchanged");
    check(fdtable_unshare(&parent) == 0, "unshare succeeds after allocation recovers");
    check(parent.files != child.files && shared->owners == 1 &&
              __atomic_load_n(&shared->refcount.value, __ATOMIC_RELAXED) == 1 &&
              vf.ref_count.value == 2,
          "successful unshare clones one file ref and decrements old owner");
    close_all_slots(&parent);
    close_all_slots(&child);
    fdtable_release_files(&parent);
    fdtable_release_files(&child);
    puts("FDTABLE_DIFF: unshare OOM unchanged-table rollback PASS");
}

static void test_share_owner_survives_source_teardown(void)
{
    task_t src, dst;
    vfile_t vf;
    int finalized_before = finalized_files;
    init_task(&src, 83);
    init_task(&dst, 84);
    init_vfile(&vf, 830);
    current_task = &src;
    check(fdtable_install_vfile(&src, &vf, 0) == 0,
          "share race source descriptor");
    files_struct_t *source_table = (files_struct_t *)src.files;
    share_teardown_source = &src;
    fdtable_share(&dst, &src);
    check(src.files == NULL && dst.files == source_table,
          "source teardown cannot invalidate acquired destination owner");
    check(source_table->owners == 1 && source_table->refcount.value == 1 &&
              source_table->fd[0] == &vf && vf.ref_count.value == 1,
          "share source teardown leaves exactly destination owner and slot ref");

    files_struct_t *self_table = source_table;
    fdtable_share(&dst, &dst);
    check(dst.files == self_table && self_table->owners == 1 &&
              self_table->refcount.value == 1,
          "self-share does not transiently drop final table owner");
    close_all_slots(&dst);
    fdtable_release_files(&dst);
    check(vf.ref_count.value == 0 && finalized_files == finalized_before + 1,
          "shared table final close releases vfile exactly once");
    puts("FDTABLE_DIFF: share owner survives source teardown/self-share PASS");
}

static void test_unlisted_clone_and_registry_teardown(void)
{
    task_t src, dst;
    vfile_t vf;
    init_task(&src, 85);
    memset(&dst, 0, sizeof(dst));
    dst.pid = 86;
    dst.limits.nofile = MAX_FILES;
    spin_init(&dst.park_lock);
    init_vfile(&vf, 850);
    current_task = &src;
    check(fdtable_install_vfile(&src, &vf, 0) == 0,
          "unlisted clone source slot installed");
    clone_race_source = &src;
    clone_race_file = &vf;
    clone_race_hook_calls = 0;
    int final_before = final_slot_hook_calls;
    check(fdtable_copy_checked(&dst, &src) == 0,
          "checked clone publishes after the last source close");
    check(clone_race_hook_calls == 1 && src.files != NULL && dst.files != NULL,
          "clone/source close interleaving hook ran once");
    check(vf.ref_count.value == 1 &&
              ((files_struct_t *)dst.files)->fd[0] == &vf &&
              final_slot_hook_calls == final_before,
          "unpublished destination ref prevents premature final-slot pruning");
    check(fdtable_file_has_open_slot(&vf),
          "published destination remains visible in registry");
    close_all_slots(&dst);
    check(vf.ref_count.value == 0 &&
              final_slot_hook_calls == final_before + 1 &&
              !fdtable_file_has_open_slot(&vf),
          "final close is notified and unlinked table is absent from scan");
    fdtable_release_files(&dst);
    fdtable_release_files(&src);
    puts("FDTABLE_DIFF: registered unpublished clone close race/teardown PASS");
}

static void test_open_slot_membership_aliases_and_fd_limit(void)
{
    task_t src, child;
    vfile_t vf;
    init_task(&src, 851);
    memset(&child, 0, sizeof(child));
    child.pid = 852;
    child.limits.nofile = MAX_FILES;
    spin_init(&child.park_lock);
    init_vfile(&vf, 8510);
    current_task = &src;
    check(fdtable_install_vfile(&src, &vf, 0) == 0,
          "membership source descriptor installed");
    check(fdtable_dup(&src, 0, MAX_FILES - 1, 0) == MAX_FILES - 1,
          "membership duplicate reaches highest valid descriptor");
    check(fdtable_file_has_open_slot(&vf),
          "membership finds same vfile through dup alias");
    check(fdtable_copy_checked(&child, &src) == 0,
          "membership fork clone succeeds");
    check(fdtable_close_files((files_struct_t *)src.files, 0, src.pid) == 0 &&
              fdtable_close_files((files_struct_t *)src.files, MAX_FILES - 1,
                                  src.pid) == 0 &&
              fdtable_file_has_open_slot(&vf),
          "fork aliases keep membership after parent slots close");
    int final_before = final_slot_hook_calls;
    check(fdtable_close_files((files_struct_t *)child.files, 0, child.pid) == 0 &&
              fdtable_file_has_open_slot(&vf),
          "highest child alias retains membership after first child close");
    check(fdtable_close_files((files_struct_t *)child.files, MAX_FILES - 1,
                              child.pid) == 0 &&
              !fdtable_file_has_open_slot(&vf) &&
              final_slot_hook_calls == final_before + 1,
          "last alias close removes membership and notifies final detach");
    fdtable_release_files(&child);
    fdtable_release_files(&src);
    puts("FDTABLE_DIFF: dup/fork aliases, upper fd bound, final detach PASS");
}

static void test_stdio_slot_ownership(void)
{
    task_t task;
    vfile_t custom;
    int finalized_before = finalized_files;
    int baseline[3];
    for (int i = 0; i < 3; i++) {
        init_vfile(&stdio_vfiles[i], (uint64_t)(900 + i));
        stdio_vfiles[i].ref_count.value = 999;
        baseline[i] = stdio_vfiles[i].ref_count.value;
    }
    init_vfile(&custom, 910);
    init_task(&task, 87);
    stdio_stub_enabled = 1;
    fdtable_init_stdio(&task);
    files_struct_t *files = (files_struct_t *)task.files;
    check(files->fd[0] == &stdio_vfiles[0] &&
              files->fd[1] == &stdio_vfiles[1] &&
              files->fd[2] == &stdio_vfiles[2] &&
              files->open_mask[0] == 7 && files->next_fd == 3,
          "task stdio init owns the three fixed slots and bitmap");
    for (int i = 0; i < 3; i++)
        check(stdio_vfiles[i].ref_count.value == baseline[i] + 1,
              "task stdio init acquires one slot reference");
    fdtable_init_stdio(&task);
    for (int i = 0; i < 3; i++)
        check(stdio_vfiles[i].ref_count.value == baseline[i] + 1,
              "repeated stdio init does not leak occupied-slot references");
    check(fdtable_close_files(files, 1, task.pid) == 0 &&
              fdtable_install_vfile(&task, &custom, 0) == 1,
          "stdio slot may be replaced by regular descriptor");
    check(fdtable_set_cloexec(&task, 1, 1) == 0,
          "custom middle descriptor cloexec set");
    fdtable_init_stdio(&task);
    check(files->fd[1] == &custom && files->cloexec[1] == 1 &&
              custom.ref_count.value == 1,
          "stdio init preserves occupied custom slot and cloexec metadata");
    fdtable_release_files(&task);
    check(custom.ref_count.value == 0 && finalized_files == finalized_before + 1,
          "task table teardown releases custom slot exactly once");
    for (int i = 0; i < 3; i++)
        check(stdio_vfiles[i].ref_count.value == baseline[i],
              "task teardown returns static stdio refs to baseline");

    current_task = NULL;
    stdio_stub_enabled = 1;
    check(fdtable_get_current(0) == 0 && fdtable_get_current(1) == 1 &&
              fdtable_get_current(2) == 2,
          "boot table lazily initializes fixed stdio descriptors");
    check(fdtable_file_has_open_slot(&stdio_vfiles[0]) &&
              stdio_vfiles[0].ref_count.value == baseline[0] + 1 &&
              stdio_vfiles[1].ref_count.value == baseline[1] + 1 &&
              stdio_vfiles[2].ref_count.value == baseline[2] + 1,
          "boot table participates in registry without task publication");
    current_task = &task;
    stdio_stub_enabled = 0;
    puts("FDTABLE_DIFF: task and boot stdio fixed-slot ownership PASS");
}

static void test_fdtable_init_replaces_registered_tables(void)
{
    task_t task;
    vfile_t private_vf, shared_vf;
    init_task(&task, 88);
    init_vfile(&private_vf, 920);
    check(fdtable_install_vfile(&task, &private_vf, 0) == 0,
          "fdtable_init replacement private slot installed");
    files_struct_t *private_old = (files_struct_t *)task.files;
    expected_table_free = private_old;
    expected_table_free_count = 0;
    fdtable_init(&task);
    check(task.files != private_old && expected_table_free_count == 1 &&
              private_vf.ref_count.value == 0 &&
              !fdtable_file_has_open_slot(&private_vf),
          "fdtable_init finalizes/unlinks an occupied private old table");
    expected_table_free = NULL;

    task_t sibling;
    init_task(&sibling, 89);
    init_vfile(&shared_vf, 921);
    check(fdtable_install_vfile(&task, &shared_vf, 0) == 0,
          "fdtable_init replacement shared slot installed");
    fdtable_share(&sibling, &task);
    files_struct_t *shared_old = (files_struct_t *)task.files;
    check(shared_old == sibling.files && shared_old->owners == 2,
          "fdtable_init shared replacement baseline");
    expected_table_free = shared_old;
    expected_table_free_count = 0;
    fdtable_init(&task);
    check(task.files != shared_old && sibling.files == shared_old &&
              shared_old->owners == 1 && shared_old->refcount.value == 1 &&
              expected_table_free_count == 0 &&
              shared_vf.ref_count.value == 1 &&
              fdtable_file_has_open_slot(&shared_vf),
          "fdtable_init drops only its owner of a shared registered table");
    fdtable_release_files(&sibling);
    check(expected_table_free_count == 1 && shared_vf.ref_count.value == 0 &&
              !fdtable_file_has_open_slot(&shared_vf),
          "last shared owner teardown unlinks and finalizes old table");
    expected_table_free = NULL;
    fdtable_release_files(&task);
    puts("FDTABLE_DIFF: fdtable_init occupied/shared replacement PASS");
}

static void test_bulk_close_on_exec_allocation_paths(void)
{
    for (int fail_alloc = 0; fail_alloc <= 1; fail_alloc++) {
        task_t task;
        vfile_t a, b, c;
        init_task(&task, 60 + fail_alloc);
        init_vfile(&a, (uint64_t)(200 + fail_alloc * 3));
        init_vfile(&b, (uint64_t)(201 + fail_alloc * 3));
        init_vfile(&c, (uint64_t)(202 + fail_alloc * 3));
        int afd = fdtable_install_vfile(&task, &a, 0);
        int bfd = fdtable_install_vfile(&task, &b, 0);
        int cfd = fdtable_install_vfile(&task, &c, 0);
        check(afd == 0 && bfd == 1 && cfd == 2,
              "bulk close-on-exec slots installed");
        check(fdtable_set_cloexec(&task, afd, 1) == 0 &&
                  fdtable_set_cloexec(&task, cfd, 1) == 0,
              "bulk close-on-exec subset flagged");
        if (fail_alloc)
            fail_next_kmalloc = 1;
        fdtable_close_on_exec(&task);
        files_struct_t *files = (files_struct_t *)task.files;
        check(files->fd[afd] == NULL && files->fd[bfd] == &b &&
                  files->fd[cfd] == NULL && files->open_mask[0] == (1u << bfd),
              "close-on-exec detaches only the selected subset");
        check(a.ref_count.value == 0 && b.ref_count.value == 1 &&
                  c.ref_count.value == 0,
              "close-on-exec drops exactly selected slot refs");
        check(fdtable_close_files(files, bfd, task.pid) == 0 &&
                  b.ref_count.value == 0,
              "close remaining slot after close-on-exec");
        expected_table_free = files;
        expected_table_free_count = 0;
        fdtable_release_files(&task);
        check(task.files == NULL && expected_table_free_count == 1,
              "close-on-exec table teardown frees files_struct");
        expected_table_free = NULL;
    }
    puts("FDTABLE_DIFF: close-on-exec subset normal/OOM paths PASS");
}

static void test_full_teardown_allocation_paths(void)
{
    for (int fail_alloc = 0; fail_alloc <= 1; fail_alloc++) {
        task_t task;
        vfile_t a, b, c;
        init_task(&task, 70 + fail_alloc);
        init_vfile(&a, (uint64_t)(300 + fail_alloc * 3));
        init_vfile(&b, (uint64_t)(301 + fail_alloc * 3));
        init_vfile(&c, (uint64_t)(302 + fail_alloc * 3));
        check(fdtable_install_vfile(&task, &a, 0) == 0 &&
                  fdtable_install_vfile(&task, &b, 0) == 1 &&
                  fdtable_install_vfile(&task, &c, 0) == 2,
              "teardown slots installed");
        check(fdtable_dup(&task, 0, 64, 0) == 64 &&
                  a.ref_count.value == 2,
              "teardown duplicate holds an additional slot ref");
        files_struct_t *files = (files_struct_t *)task.files;
        expected_table_free = files;
        expected_table_free_count = 0;
        if (fail_alloc)
            fail_next_kmalloc = 1;
        fdtable_release_files(&task);
        check(task.files == NULL && expected_table_free_count == 1,
              "normal/OOM final teardown frees files_struct exactly once");
        check(a.ref_count.value == 0 && b.ref_count.value == 0 &&
                  c.ref_count.value == 0,
              "normal/OOM final teardown drops every slot ref exactly once");
        expected_table_free = NULL;
    }
    puts("FDTABLE_DIFF: full teardown normal/OOM paths PASS");
}

static void test_corrupt_bulk_state_is_atomic(void)
{
    task_t task;
    vfile_t a, b;
    init_task(&task, 80);
    init_vfile(&a, 400);
    init_vfile(&b, 401);
    int afd = fdtable_install_vfile(&task, &a, 0);
    int bfd = fdtable_install_vfile(&task, &b, 0);
    check(afd == 0 && bfd == 1, "corruption test slots installed");
    check(fdtable_set_cloexec(&task, afd, 1) == 0,
          "corruption test has selected cloexec slot");
    files_struct_t *files = (files_struct_t *)task.files;

    files->open_mask[0] &= ~(1u << bfd); /* occupied slot without mask bit */
    panic_expected = 1;
    if (setjmp(panic_jmp) == 0) {
        fdtable_close_on_exec(&task);
        check(false, "close-on-exec rejects non-null slot with clear bit");
    }
    check(files->fd[afd] == &a && files->fd[bfd] == &b &&
              a.ref_count.value == 1 && b.ref_count.value == 1,
          "corrupt slot/bitmap leaves valid prefix and refs unchanged");
    files->open_mask[0] |= 1u << bfd;

    files->open_mask[0] |= 1u << 2; /* set bit without a VFile */
    panic_expected = 1;
    if (setjmp(panic_jmp) == 0) {
        fdtable_close_on_exec(&task);
        check(false, "close-on-exec rejects set bit without a VFile");
    }
    check(files->fd[afd] == &a && files->fd[bfd] == &b &&
              a.ref_count.value == 1 && b.ref_count.value == 1,
          "corrupt bitmap leaves slots and refs unchanged");
    files->open_mask[0] &= ~(1u << 2);
    close_all_slots(&task);
    fdtable_release_files(&task);
    check(a.ref_count.value == 0 && b.ref_count.value == 0,
          "repair permits clean table release after corruption checks");
    puts("FDTABLE_DIFF: corrupted bulk state rejected atomically PASS");
}

static void *install_during_closeexec(void *unused)
{
    (void)unused;
    pthread_mutex_lock(&close_race_lock);
    while (!close_race_notified)
        pthread_cond_wait(&close_race_cond, &close_race_lock);
    pthread_mutex_unlock(&close_race_lock);

    int fd = fdtable_install_vfile(close_race_task, close_race_replacement, 0);
    pthread_mutex_lock(&close_race_lock);
    close_race_installed_fd = fd;
    close_race_install_done = 1;
    pthread_cond_broadcast(&close_race_cond);
    pthread_mutex_unlock(&close_race_lock);
    return NULL;
}

static void test_oom_closeexec_concurrent_reuse(void)
{
    task_t task;
    vfile_t first, middle, last, replacement;
    init_task(&task, 81);
    init_vfile(&first, 500);
    init_vfile(&middle, 501);
    init_vfile(&last, 502);
    init_vfile(&replacement, 503);
    check(fdtable_install_vfile(&task, &first, 0) == 0 &&
              fdtable_install_vfile(&task, &middle, 0) == 1 &&
              fdtable_install_vfile(&task, &last, 0) == 2,
          "concurrent OOM closeexec slots installed");
    check(fdtable_set_cloexec(&task, 0, 1) == 0 &&
              fdtable_set_cloexec(&task, 2, 1) == 0,
          "concurrent OOM closeexec flags set");
    close_race_vfile = &first;
    close_race_task = &task;
    close_race_replacement = &replacement;
    close_race_notified = 0;
    close_race_installed_fd = -1;
    close_race_install_done = 0;
    pthread_t installer;
    check(pthread_create(&installer, NULL, install_during_closeexec, NULL) == 0,
          "spawn concurrent closeexec descriptor reuse");
    fail_next_kmalloc = 1;
    fdtable_close_on_exec(&task);
    check(pthread_join(installer, NULL) == 0,
          "join concurrent closeexec descriptor reuse");
    close_race_vfile = NULL;
    files_struct_t *files = (files_struct_t *)task.files;
    check(close_race_installed_fd == 0 && files->fd[0] == &replacement &&
              files->fd[1] == &middle && files->fd[2] == NULL,
          "OOM closeexec keeps concurrent reused slot and closes remaining flagged slot");
    check(first.ref_count.value == 0 && middle.ref_count.value == 1 &&
              last.ref_count.value == 0 && replacement.ref_count.value == 1,
          "OOM closeexec concurrent interleaving balances slot refs");
    close_all_slots(&task);
    fdtable_release_files(&task);
    check(first.ref_count.value == 0 && middle.ref_count.value == 0 &&
              last.ref_count.value == 0 && replacement.ref_count.value == 0,
          "concurrent OOM closeexec follow-up closes remaining refs");
    puts("FDTABLE_DIFF: OOM close-on-exec concurrent reuse PASS");
}

typedef struct {
    task_t *task;
    vfile_t *vf;
    int result;
} alloc_arg_t;

static void *alloc_thread(void *opaque)
{
    alloc_arg_t *arg = opaque;
    arg->result = fdtable_install_vfile(arg->task, arg->vf, 0);
    return NULL;
}

static void *dup_close_churn_thread(void *opaque)
{
    alloc_arg_t *arg = opaque;
    if (arg->result >= 0) {
        for (int i = 0; i < 64; i++) {
            int duplicate = fdtable_dup(arg->task, arg->result, 64, 0);
            int closed = duplicate < 0 ? duplicate :
                fdtable_close_files_if_file((files_struct_t *)arg->task->files,
                                            duplicate, arg->task->pid,
                                            arg->vf);
            if (duplicate < 0 || closed < 0) {
                fprintf(stderr, "FDTABLE_DIFF: churn dup=%d close=%d\n",
                        duplicate, closed);
                arg->result = -1;
                break;
            }
        }
    }
    return NULL;
}

static void test_concurrent_allocation(void)
{
    enum { THREADS = 8 };
    task_t task;
    vfile_t files[THREADS];
    alloc_arg_t args[THREADS];
    pthread_t threads[THREADS];
    init_task(&task, 51);
    for (int i = 0; i < THREADS; i++) {
        init_vfile(&files[i], (uint64_t)(100 + i));
        args[i] = (alloc_arg_t){ .task = &task, .vf = &files[i], .result = -1 };
        check(pthread_create(&threads[i], NULL, alloc_thread, &args[i]) == 0,
              "spawn concurrent allocator");
    }
    for (int i = 0; i < THREADS; i++)
        check(pthread_join(threads[i], NULL) == 0, "join allocator");
    unsigned seen = 0;
    for (int i = 0; i < THREADS; i++) {
        check(args[i].result >= 0 && args[i].result < THREADS,
              "concurrent allocation result");
        unsigned bit = 1u << args[i].result;
        check(!(seen & bit), "concurrent allocations are unique");
        seen |= bit;
    }
    check(seen == (1u << THREADS) - 1, "all concurrent slots allocated");
    for (int i = 0; i < THREADS; i++)
        check(pthread_create(&threads[i], NULL, dup_close_churn_thread,
                             &args[i]) == 0,
              "spawn concurrent dup/close churn");
    for (int i = 0; i < THREADS; i++)
        check(pthread_join(threads[i], NULL) == 0,
              "join dup/close churn");
    close_all_slots(&task);
    for (int i = 0; i < THREADS; i++)
        check(files[i].ref_count.value == 0, "concurrent slot refs released");
    fdtable_release_files(&task);
    puts("FDTABLE_DIFF: concurrent allocation/dup/conditional-close churn PASS");
}

int main(void)
{
    spin_init(&tasklist_lock);
    test_lifecycle();
    test_checked_copy_failure_atomicity();
    test_unshare_allocation_failure_preserves_shared_table();
    test_share_owner_survives_source_teardown();
    test_unlisted_clone_and_registry_teardown();
    test_open_slot_membership_aliases_and_fd_limit();
    test_stdio_slot_ownership();
    test_fdtable_init_replaces_registered_tables();
    test_bulk_close_on_exec_allocation_paths();
    test_full_teardown_allocation_paths();
    test_corrupt_bulk_state_is_atomic();
    test_oom_closeexec_concurrent_reuse();
    test_concurrent_allocation();
    {
        task_t task;
        vfile_t vf;
        init_task(&task, 99);
        init_vfile(&vf, 990);
        current_task = &task;
        check(fdtable_install_vfile(&task, &vf, 0) == 0,
              "close-error test installs owned descriptor");
        vfile_get(&vf);
        check(fdtable_install_vfile(&task, &vf, 0) == 1,
              "close-error test installs second reference");
        finalizer_error = -EIO;
        int finalized_before = finalized_files;
        check(fdtable_close_files((files_struct_t *)task.files, 0, task.pid)
                      == 0,
              "non-final slot close does not report deferred close error");
        check(((files_struct_t *)task.files)->fd[0] == NULL &&
                  ((files_struct_t *)task.files)->fd[1] == &vf &&
                  vf.ref_count.value == 1 && finalized_files == finalized_before,
              "non-final close detaches only its slot");
        check(fdtable_close_files((files_struct_t *)task.files, 1, task.pid)
                      == -EIO,
              "last-slot close propagates finalizer errno");
        check(((files_struct_t *)task.files)->fd[1] == NULL &&
                  vf.ref_count.value == 0 &&
                  finalized_files == finalized_before + 1,
              "close error consumes detached descriptor exactly once");
        finalizer_error = 0;
        fdtable_release_files(&task);
        current_task = NULL;
    }
    check(finalized_files == 42, "finalizer called once per owned file");
#ifdef CONFIG_FDTABLE_STATE_RUST
    puts("FDTABLE_DIFF: rust-selected production fdtable.c PASS");
#else
    puts("FDTABLE_DIFF: C production fdtable.c PASS");
#endif
    return 0;
}

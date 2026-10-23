/*
 * A20OS Native ABI — Phase 1 syscall implementations.
 *
 * Implemented with real kernel API backing:
 *   abi_info, feature_test,
 *   handle_close/dup/query/replace/close_many/seek,
 *   task_exit, task_wait,
 *   path_open, handle_read, handle_write, handle_stat,
 *   clock_get.
 *
 * Current limitations: feature_test can report unsupported optional features;
 * Debug ABI calls in sys_phase2.c are intentionally limited compatibility
 * implementations until ptrace-grade debug semantics are specified.
 *
 * Design references (see docs/native-abi/):
 *   03-handle.md §4 — syscall operation semantics
 *   04-memory.md §4 — vm_alloc/unmap semantics
 *   06-security.md §3.2 — operation-rights mapping
 *   07-startup.md §1.4 — initial handle rights
 */
#include "core/types.h"
#include "core/klog.h"
#include "core/defs.h"
#include "core/string.h"
#include "core/consts.h"
#include "core/mman.h"
#include "core/stdio.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "handle_table.h"
#include "proc/proc.h"
#include "proc/exec.h"
#include "proc/signal.h"
#include "uapi/a20/exec.h"
#include "uapi/a20/thread_exit.h"
#include "proc/proc_internal.h"
#include "mm/mm.h"
#include "mm/elf.h"
#include "mm/vm.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "sys/usercopy.h"

#include "abi/core_api.h"
#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/startup.h"
#include "abi/linux/fcntl.h"
#include "abi/linux/errno.h"
#include "sys_validate.h"
#include "uapi/a20/abi_features.h"

#define A20_ARG(n) (args->arg[(n)])

/* Internal handle table functions (from handle_table.c) */
struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);
void a20_ht_put_ref(struct a20_ht_internal *ht);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                            uint16_t type, a20_rights_t rights);
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                     uint16_t type, a20_rights_t rights,
                                     uint64_t expiry_tick, uint32_t remaining_ops,
                                     uint32_t temporal_flags, uint8_t security_label);
int64_t a20_handle_install_at_temporal(struct a20_ht_internal *ht,
                                       a20_handle_t slot, void *object,
                                       uint16_t type, a20_rights_t rights,
                                       uint64_t expiry_tick,
                                       uint32_t remaining_ops,
                                       uint32_t temporal_flags,
                                       uint8_t security_label);
int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                    uint16_t expected_type, a20_rights_t required_rights,
                                    a20_handle_entry_t *out);
int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                       a20_handle_t h,
                                       uint16_t expected_type,
                                       a20_rights_t required_rights,
                                       a20_handle_entry_t *out);
int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
void a20_object_ref(void *object, uint16_t type);
void a20_object_release(void *object, uint16_t type);
struct a20_ht_internal *task_get_a20_ht(task_t *t);
uint8_t a20_ht_get_label(struct a20_ht_internal *ht);

/* ===== Core (0x0000) ===== */

static uint64_t native_feature_word0(void)
{
    uint64_t features = (1ULL << 0) | (1ULL << 1) | (1ULL << 2) | (1ULL << 3) |
        (1ULL << A20_ABI_FEATURE_BYTE_STREAM) | (1ULL << A20_ABI_FEATURE_MAP_MODES) |
        (1ULL << A20_ABI_FEATURE_FILE_FLAGS) | (1ULL << A20_ABI_FEATURE_SOCKET_OPTIONS) |
        (1ULL << A20_ABI_FEATURE_TERMINAL_CONTROL) | (1ULL << A20_ABI_FEATURE_FILE_LOCKS) |
        (1ULL << A20_ABI_FEATURE_NET_MESSAGES_V2) | (1ULL << A20_ABI_FEATURE_THREAD_JOINABLE);
    /* The opt-in tag must fit a scalar register. 32-bit profiles keep legacy
     * calls and must not advertise extensions whose tag would be truncated. */
    if (sizeof(uintptr_t) == 8)
        features |= (1ULL << A20_ABI_FEATURE_EXEC_BOOTSTRAP) |
                    (1ULL << A20_ABI_FEATURE_THREAD_EXIT_CLEANUP) |
                    (1ULL << A20_ABI_FEATURE_SIGNAL_EXIT);
#ifdef CONFIG_ABI_BOTH
    features |= 1ULL << A20_ABI_FEATURE_LINUX_EXEC;
#endif
    return features;
}

int64_t sys_a20_abi_info(const a20_syscall_args_t *args)
{
    a20_abi_info_t *out = (a20_abi_info_t *)(uintptr_t)A20_ARG(0);
    if (!out) return -A20_ERR_FAULT;

    a20_abi_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = 1;
    info.abi_major = A20_ABI_MAJOR;
    info.abi_minor = A20_ABI_MINOR;
    info.abi_patch = A20_ABI_PATCH;
    info.pointer_bits = sizeof(void *) * 8;
    info.page_size = 4096;
    info.handle_bits = 32;
    /* feature_bits: bit 0 = handle_table, bit 1 = temporal_capabilities,
     * bit 2 = futex (Sync 0x0B00), bit 3 = atomic32 (0x0B02) */
    info.feature_bits[0] = native_feature_word0();
    if (copy_to_user(out, &info, sizeof(info)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_feature_test(const a20_syscall_args_t *args)
{
    uint64_t feature_id = A20_ARG(0);
    uint64_t word = feature_id / 64;
    uint64_t bit  = feature_id % 64;
    if (word >= 4) return -A20_ERR_NOT_SUPPORTED;

    /* Match the feature_bits from abi_info */
    uint64_t features[4] = {0};
    features[0] = native_feature_word0();
    if (features[word] & (1ULL << bit))
        return A20_OK;
    return -A20_ERR_NOT_SUPPORTED;
}

/* ===== Handle (0x0100) ===== */

int64_t sys_a20_handle_close(const a20_syscall_args_t *args);

int64_t sys_a20_handle_dup(const a20_syscall_args_t *args)
{
    a20_handle_dup_args_t *uargs = (a20_handle_dup_args_t *)(uintptr_t)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_handle_dup_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t src;
    int64_t ret = a20_handle_lookup_ref_internal(ht, kargs.source,
                                                 A20_OBJ_INVALID, A20_RIGHT_DUP,
                                                 &src);
    if (ret < 0) return ret;

    /* rights_mask must be subset of source rights docs/native-abi/06-security.md §2.2) */
    if (kargs.rights_mask != 0 && (kargs.rights_mask & ~src.rights) != 0) {
        a20_object_release(src.object, src.type);
        return -A20_ERR_ACCESS;
    }

    a20_rights_t new_rights = kargs.rights_mask ? kargs.rights_mask : src.rights;
    int64_t new_h = a20_handle_install_temporal(ht, src.object, src.type, new_rights,
                                                src.expiry_tick, src.remaining_ops,
                                                src.temporal_flags, src.security_label);
    if (new_h < 0) {
        a20_object_release(src.object, src.type);
        return new_h;
    }

    /* The new handle owns one object reference; the lookup reference keeps
     * the object alive until this second reference has been taken. */
    a20_object_ref(src.object, src.type);
    a20_object_release(src.object, src.type);

    kargs.out_handle = (a20_handle_t)new_h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)new_h);
        return -A20_ERR_FAULT;
    }

    return A20_OK;
}

int64_t sys_a20_handle_query(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_handle_info_t *out = (a20_handle_info_t *)(uintptr_t)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                              A20_RIGHT_STAT, &entry);
    if (ret < 0) return ret;

    a20_handle_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = 1;
    info.object_type = entry.type;
    info.rights = entry.rights;
    if (copy_to_user(out, &info, sizeof(info)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_handle_replace(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_rights_t rights = (a20_rights_t)A20_ARG(1);
    a20_handle_t *out = (a20_handle_t *)(uintptr_t)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t src;
    int64_t ret = a20_handle_lookup_ref_internal(ht, h, A20_OBJ_INVALID,
                                                 A20_RIGHT_DUP, &src);
    if (ret < 0) return ret;

    if (rights != 0 && (rights & ~src.rights) != 0) {
        a20_object_release(src.object, src.type);
        return -A20_ERR_ACCESS;
    }

    a20_rights_t new_rights = rights ? rights : src.rights;
    int64_t new_h = a20_handle_install_temporal(ht, src.object, src.type, new_rights,
                                                src.expiry_tick, src.remaining_ops,
                                                src.temporal_flags, src.security_label);
    if (new_h < 0) {
        a20_object_release(src.object, src.type);
        return new_h;
    }

    /* The new entry takes a reference while the lookup reference still pins
     * the object; removing the old handle then drops the old entry's ref. */
    a20_object_ref(src.object, src.type);
    a20_object_release(src.object, src.type);
    a20_handle_remove(ht, h);

    if (out) {
        a20_handle_t result = (a20_handle_t)new_h;
        if (copy_to_user(out, &result, sizeof(result)) < 0) {
            a20_handle_remove(ht, (a20_handle_t)new_h);
            return -A20_ERR_FAULT;
        }
    }

    return A20_OK;
}

int64_t sys_a20_handle_close_many(const a20_syscall_args_t *args)
{
    uint64_t handles_ptr = A20_ARG(0);
    uint32_t count = (uint32_t)A20_ARG(1);
    a20_handle_t *handles = (a20_handle_t *)(uintptr_t)handles_ptr;
    if (!handles || count > 4096) return -A20_ERR_FAULT;

    uint32_t closed = 0;
    uint32_t done = 0;
    a20_handle_t khandles[64];
    while (done < count) {
        uint32_t n = count - done;
        if (n > 64) n = 64;
        if (copy_from_user(khandles, handles + done,
                           n * sizeof(a20_handle_t)) < 0)
            break;
        for (uint32_t i = 0; i < n; i++) {
            a20_syscall_args_t single = { .arg = { khandles[i] } };
            if (sys_a20_handle_close(&single) >= 0)
                closed++;
        }
        done += n;
    }
    return (int64_t)closed;
}

/* ===== Task (0x0200) ===== */

int64_t sys_a20_task_exit(const a20_syscall_args_t *args)
{
    int32_t code = (int32_t)A20_ARG(0);
    if (A20_ARG(0) == A20_EXIT_STRUCT) {
        a20_exit_args_t request;
        a20_exit_args_t *user = (void *)(uintptr_t)A20_ARG(1);
        if (!user || copy_from_user(&request, user, sizeof(request)) < 0)
            return -A20_ERR_FAULT;
        if (request.size < sizeof(request) || request.version != 1 ||
            request.flags || request.release_base || request.release_length ||
            request.cause > A20_EXIT_CAUSE_NOTIFICATION ||
            (request.cause == A20_EXIT_CAUSE_NORMAL && request.notification) ||
            (request.cause == A20_EXIT_CAUSE_NOTIFICATION &&
             (!request.notification || request.notification >= NSIG)))
            return -A20_ERR_INVALID_ARGUMENT;
        code = request.cause == A20_EXIT_CAUSE_NOTIFICATION ?
            signal_fatal_exit_code_dumped(request.notification, 0) : request.code;
    }
    task_t *cur = proc_current();

    /* Drop this thread's handle-table reference; the table outlives until
     * the last thread of the process exits (process-local semantics). */
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (ht) {
        struct a20_ht_internal *owned = (struct a20_ht_internal *)
            __atomic_exchange_n(&cur->a20_ht, NULL, __ATOMIC_ACQ_REL);
        if (owned) a20_ht_put_ref(owned);
    }

    /* task_exit terminates the whole process (all threads), matching the
     * design intent that a task is the process container. */
    proc_exit_group(code);
    return 0;
}

/* ------------------------------------------------------------------ */
/*  task_spawn argv/envp: copy into kernel buffers before use         */
/* ------------------------------------------------------------------ */

/*
 * Copy a user argv/envp pointer array *and* the strings it points to into
 * kernel-owned buffers.  elf_setup_stack_a20() reads the strings with plain
 * strlen()/memcpy(), so the source must be kernel memory; passing raw user
 * pointers faults when the backing page is not mapped in the current
 * address space (observed as a kernel strlen page fault on the child's
 * stack top during posix_spawn).
 *
 * @src     user pointer to the string-pointer array, or NULL
 * @out     pre-zeroed array of char* with room for max_count + 1 entries
 * @max_count  maximum number of strings to copy
 *
 * Returns the number of entries copied (>= 0), or -1 on failure.  On
 * failure any allocated strings are freed; out is NULL-terminated on
 * success.  Caller frees the strings with free_arg_vector().
 */
static int copy_arg_vector(char *const *src, char **out, int max_count)
{
    int count = 0;

    if (!src) {
        out[0] = NULL;
        return 0;
    }

    while (count < max_count) {
        char *ptr;
        if (copy_from_user(&ptr, &src[count], sizeof(char *)) < 0)
            break;
        if (!ptr)
            break;

        long measured = user_strnlen(ptr, MAX_ARG_STRLEN - 1);
        if (measured < 0 || measured >= (long)(MAX_ARG_STRLEN - 1))
            break;
        size_t len = (size_t)measured + 1;
        char *copy = kmalloc(len);
        if (!copy)
            break;
        if (copy_from_user(copy, ptr, len) < 0 || copy[len - 1] != '\0') {
            kfree(copy);
            break;
        }
        out[count] = copy;
        count++;
    }

    /* Detect overflow: if the array has more than max_count entries. */
    if (count == max_count) {
        char *extra;
        if (copy_from_user(&extra, &src[count], sizeof(char *)) == 0 && extra)
            count = -1;
    }

    if (count < 0) {
        for (int i = 0; i < max_count && out[i]; i++) {
            kfree(out[i]);
            out[i] = NULL;
        }
        return -1;
    }

    out[count] = NULL;
    return count;
}

static void free_arg_vector(char **vec, int count)
{
    for (int i = 0; i < count; i++) {
        kfree(vec[i]);
        vec[i] = NULL;
    }
}

#include "spawn_params.h"

int64_t sys_a20_task_spawn(const a20_syscall_args_t *args)
{
    a20_task_spawn_args_t *uargs = (a20_task_spawn_args_t *)(uintptr_t)A20_ARG(0);
    a20_task_spawn_args_t kargs;
    int validated = a20_spawn_copy_params(uargs, &kargs);
    if (validated)
        return validated;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t img_entry;
    int64_t r = a20_handle_lookup_ref_internal(ht, kargs.image, A20_OBJ_FILE,
                                               A20_RIGHT_READ | A20_RIGHT_EXEC,
                                               &img_entry);
    if (r < 0) return r;

    vfile_t *img_vf = (vfile_t *)img_entry.object;
    if (!img_vf) {
        a20_object_release(img_entry.object, img_entry.type);
        return -A20_ERR_BAD_HANDLE;
    }

    char path_buf[MAX_PATH_LEN];
    strncpy(path_buf, img_vf->path, MAX_PATH_LEN);

    /* The ELF loader reads through an fd; publish the image vfile in this
     * task's table for the duration of the load.  The install consumes the
     * lookup reference. */
    int img_fd = fdtable_install_current_vfile(img_vf, 0);
    if (img_fd < 0) {
        a20_object_release(img_entry.object, img_entry.type);
        return -A20_ERR_BAD_HANDLE;
    }

    elf_load_info_t info;
    memset(&info, 0, sizeof(info));
    r = elf_load(img_fd, path_buf, &info);
    /* Closing the temporary fd releases the consumed lookup reference.
     * The image handle and lazy ELF VMAs retain their own references. */
    fdtable_close_current(img_fd);
    /* fdtable_close_current already released the consumed lookup reference.
     * The original image handle must remain usable for another spawn. */

    if (r < 0) return -A20_ERR_IO;
    if (!info.is_native_abi) {
        elf_load_info_discard(&info);
        return -A20_ERR_NOT_SUPPORTED;
    }

    /* Copy argv/envp strings into kernel buffers while the parent address
     * space is still authoritative.  elf_setup_stack_a20() reads them with
     * raw strlen()/memcpy(), so raw user pointers would fault. */
    char *argv_ks[MAX_ARG_STRINGS + 1];
    char *envp_ks[MAX_ARG_STRINGS + 1];
    memset(argv_ks, 0, sizeof(argv_ks));
    memset(envp_ks, 0, sizeof(envp_ks));

    int argc = 0, envc = 0;
    if (kargs.argc > MAX_ARG_STRINGS || kargs.envc > MAX_ARG_STRINGS) {
        elf_load_info_discard(&info);
        return -A20_ERR_INVALID_ARGUMENT;
    }
    if (kargs.argc > 0 && kargs.argv) {
        argc = copy_arg_vector((char *const *)(uintptr_t)kargs.argv, argv_ks,
                               MAX_ARG_STRINGS);
        if (argc < 0 || argc != (int)kargs.argc) {
            free_arg_vector(argv_ks, argc > 0 ? argc : 0);
            elf_load_info_discard(&info);
            return argc < 0 ? -A20_ERR_FAULT : -A20_ERR_INVALID_ARGUMENT;
        }
    }
    if (kargs.envc > 0 && kargs.envp) {
        envc = copy_arg_vector((char *const *)(uintptr_t)kargs.envp, envp_ks,
                               MAX_ARG_STRINGS);
        if (envc < 0 || envc != (int)kargs.envc) {
            free_arg_vector(argv_ks, argc);
            free_arg_vector(envp_ks, envc > 0 ? envc : 0);
            elf_load_info_discard(&info);
            return envc < 0 ? -A20_ERR_FAULT : -A20_ERR_INVALID_ARGUMENT;
        }
    }

    size_t total_vm = (size_t)(info.end_va - info.base);
    int new_pid = proc_alloc_user_image(info.entry, info.stack_top, info.pgdir,
                                          info.mmap, info.brk,
                                          info.stack_top, total_vm,
                                          info.tls_tp
#ifdef CONFIG_NOMMU
                                          , info.nommu_allocs,
                                          info.nommu_alloc_sizes,
                                          info.nommu_alloc_types,
                                          info.num_nommu_allocs
#endif
                                           , 1);

    if (new_pid < 0) {
        elf_load_info_discard(&info);
        return -A20_ERR_NO_MEMORY;
    }

    task_t *new_task = proc_find_get(new_pid);
    if (!new_task) return -A20_ERR_NO_MEMORY;

    new_task->abi_mode = 1;
    if (kargs.flags & A20_SPAWN_PARENT_BOUND) {
        uint64_t flags = spin_lock_irqsave(&new_task->park_lock);
        new_task->pdeathsig = 9;
        spin_unlock_irqrestore(&new_task->park_lock, flags);
    }

    struct a20_ht_internal *new_ht = a20_ht_create();
    if (!new_ht) {
        proc_force_exit(new_task, 1);
        proc_make_ready(new_task);
        proc_put(new_task);
        return -A20_ERR_NO_MEMORY;
    }
    __atomic_store_n(&new_task->a20_ht, new_ht, __ATOMIC_RELEASE);

    a20_handle_entry_t root_entry;
    a20_handle_t child_root_h = A20_HANDLE_NULL;
    if (a20_handle_lookup_ref_internal(ht, kargs.root_dir, A20_OBJ_DIRECTORY,
                                       A20_RIGHT_READ | A20_RIGHT_STAT,
                                       &root_entry) == A20_OK) {
        a20_object_ref(root_entry.object, root_entry.type);
        int64_t root_h = a20_handle_install(new_ht, root_entry.object,
                                            A20_OBJ_DIRECTORY,
                                            A20_RIGHT_READ | A20_RIGHT_STAT |
                                            A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
        if (root_h < 0)
            a20_object_release(root_entry.object, root_entry.type);
        else
            child_root_h = (a20_handle_t)root_h;
        a20_object_release(root_entry.object, root_entry.type);
    }

    /* cwd_dir: fall back to the root handle when absent or invalid */
    a20_handle_t child_cwd_h = A20_HANDLE_NULL;
    if (kargs.cwd_dir != A20_HANDLE_NULL &&
        a20_handle_lookup_ref_internal(ht, kargs.cwd_dir, A20_OBJ_DIRECTORY,
                                       A20_RIGHT_READ | A20_RIGHT_STAT,
                                       &root_entry) == A20_OK) {
        a20_object_ref(root_entry.object, root_entry.type);
        int64_t cwd_h = a20_handle_install(new_ht, root_entry.object,
                                           A20_OBJ_DIRECTORY,
                                           A20_RIGHT_READ | A20_RIGHT_STAT |
                                           A20_RIGHT_DUP);
        if (cwd_h < 0)
            a20_object_release(root_entry.object, root_entry.type);
        else
            child_cwd_h = (a20_handle_t)cwd_h;
        a20_object_release(root_entry.object, root_entry.type);
    }
    if (child_cwd_h == A20_HANDLE_NULL)
        child_cwd_h = child_root_h;

    /* Keep the kernel's path-based filesystem view aligned with the cwd
     * capability.  Native libc resolves most operations through the handle,
     * but procfs, absolute path helpers, and getcwd still use task->fs.cwd. */
    a20_handle_t cwd_source = kargs.cwd_dir != A20_HANDLE_NULL
                                  ? kargs.cwd_dir : kargs.root_dir;
    if (cwd_source != A20_HANDLE_NULL &&
        a20_handle_lookup_ref_internal(ht, cwd_source, A20_OBJ_DIRECTORY,
                                       A20_RIGHT_READ | A20_RIGHT_STAT,
                                       &root_entry) == A20_OK) {
        vfile_t *cwd_vf = (vfile_t *)root_entry.object;
        if (cwd_vf && cwd_vf->path[0]) {
            strncpy(new_task->fs.cwd, cwd_vf->path, MAX_PATH_LEN - 1);
            new_task->fs.cwd[MAX_PATH_LEN - 1] = '\0';
        }
        a20_object_release(root_entry.object, root_entry.type);
    }

    /* v2: inherit stdio handles into the child's start_info slots */
    a20_handle_t child_stdio[3] = { A20_HANDLE_NULL, A20_HANDLE_NULL,
                                    A20_HANDLE_NULL };
    const a20_handle_t want_stdio[3] = { kargs.stdin_handle,
                                         kargs.stdout_handle,
                                         kargs.stderr_handle };
    for (int i = 0; i < 3; i++) {
        if (want_stdio[i] == A20_HANDLE_NULL)
            continue;
        a20_handle_entry_t src;
        if (a20_handle_lookup_ref_internal(ht, want_stdio[i], A20_OBJ_INVALID,
                                           A20_RIGHT_DUP, &src) < 0)
            continue;
        a20_object_ref(src.object, src.type);
        int64_t sh = a20_handle_install(new_ht, src.object, src.type,
                                        src.rights);
        a20_object_release(src.object, src.type);
        if (sh < 0) {
            a20_object_release(src.object, src.type);
            continue;
        }
        child_stdio[i] = (a20_handle_t)sh;
    }

    /* ---- Transfer handle array from parent to child (spawn.md §3.4) ---- */
    uint32_t child_fd_limit = 0;
    if (kargs.handle_count > 0 && kargs.handles) {
        uint32_t nh = kargs.handle_count;
        if (nh > 64) nh = 64;
        a20_spawn_handle_t sh_buf[64];
        if (copy_from_user(sh_buf, (void *)(uintptr_t)kargs.handles, nh * sizeof(a20_spawn_handle_t)) < 0) {
            proc_force_exit(new_task, 1);
            proc_make_ready(new_task);
            proc_put(new_task);
            return -A20_ERR_FAULT;
        }
        for (uint32_t i = 0; i < nh; i++) {
            a20_spawn_handle_t *sh = &sh_buf[i];
            /* Parent must have DUP right to transfer (rights.md §4.2) */
            a20_handle_entry_t src;
            int64_t lr = a20_handle_lookup_ref_internal(ht, sh->handle,
                                                        A20_OBJ_INVALID,
                                                        A20_RIGHT_DUP, &src);
            if (lr < 0) continue;

            /* Mask child rights to requested subset (never exceed source) */
            a20_rights_t child_rights = sh->rights
                                            ? sh->rights & src.rights
                                            : src.rights;
            if (child_rights == 0) {
                a20_object_release(src.object, src.type);
                continue;
            }

            /* Ref-count the underlying object for the child's new handle */
            a20_object_ref(src.object, src.type);

            int64_t child_h;
            if (sh->target_slot >= A20_NATIVE_FD_HANDLE_BASE &&
                sh->target_slot - A20_NATIVE_FD_HANDLE_BASE < 1024) {
                child_h = a20_handle_install_at_temporal(
                    new_ht, sh->target_slot, src.object, src.type,
                    child_rights, src.expiry_tick, src.remaining_ops,
                    src.temporal_flags, src.security_label);
                if (child_h >= 0) {
                    uint32_t fd = sh->target_slot - A20_NATIVE_FD_HANDLE_BASE;
                    if (fd + 1 > child_fd_limit)
                        child_fd_limit = fd + 1;
                }
            } else {
                child_h = a20_handle_install_temporal(
                    new_ht, src.object, src.type, child_rights,
                    src.expiry_tick, src.remaining_ops,
                    src.temporal_flags, src.security_label);
            }
            a20_object_release(src.object, src.type);
            if (child_h < 0) {
                a20_object_release(src.object, src.type);
                continue;
            }

        }
    }

    int64_t child_self_h = a20_handle_install_task(
        new_ht, new_task, A20_OBJ_TASK,
        A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT | A20_RIGHT_CONTROL |
            A20_RIGHT_DUP | A20_RIGHT_TRANSFER);

    int64_t task_h = a20_handle_install_task(
        ht, new_task, A20_OBJ_TASK,
        A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT | A20_RIGHT_CONTROL |
            A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_ADMIN |
            A20_RIGHT_READ | A20_RIGHT_WRITE);
    if (task_h < 0) {
        proc_force_exit(new_task, 1);
        proc_make_ready(new_task);
        proc_put(new_task);
        return task_h;
    }

    kargs.out_task = (a20_handle_t)task_h;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        a20_handle_remove(ht, (a20_handle_t)task_h);
        proc_force_exit(new_task, 1);
        proc_make_ready(new_task);
        proc_put(new_task);
        return -A20_ERR_FAULT;
    }

    a20_handle_t child_self_task = (child_self_h >= 0) ? (a20_handle_t)child_self_h : 0;

    /* Well-known service registry client endpoint (M3). */
    a20_handle_t child_registry = 0;
    {
        extern int64_t a20_registry_install_client(struct a20_ht_internal *ht);
        int64_t rh = a20_registry_install_client(new_ht);
        if (rh >= 0) child_registry = (a20_handle_t)rh;
    }

    uint64_t sp = elf_setup_stack_a20(info.stack_top, argc, argv_ks, envp_ks,
                                      &info, child_stdio[0], child_stdio[1],
                                      child_stdio[2], child_self_task,
                                      child_root_h, child_cwd_h,
                                      child_fd_limit, child_registry, NULL, 0);
    free_arg_vector(argv_ks, argc);
    free_arg_vector(envp_ks, envc);
    if (sp == 0) {
        proc_force_exit(new_task, 1);
        proc_make_ready(new_task);
        proc_put(new_task);
        return -A20_ERR_NO_MEMORY;
    }

    trap_context_t *trap = (trap_context_t *)(uintptr_t)
        ((uint64_t)(uintptr_t)new_task->kstack_base + KERNEL_STACK_SIZE -
         sizeof(trap_context_t));
    TRAP_CTX_SP(trap) = sp;
    TRAP_CTX_SET_ARG0(trap, sp);

    proc_make_ready(new_task);
    proc_put(new_task);
    return task_h;
}

/* ================================================================== */
/*  task_clone — capability-safe process continuation                  */
/* ================================================================== */

#define A20_CLONE_MANIFEST_MAX 1024

/* Transfer one explicit capability with all of its restrictions intact.
 * The lookup reference becomes the new table's ownership on success. */
static int64_t a20_clone_install(struct a20_ht_internal *source,
                                 struct a20_ht_internal *dest,
                                 a20_handle_t handle, uint16_t type,
                                 a20_rights_t requested)
{
    a20_handle_entry_t entry;
    int64_t result = a20_handle_lookup_ref_internal(source, handle, type,
                                                    A20_RIGHT_DUP, &entry);
    if (result < 0) return result;
    if (requested && (requested & ~entry.rights)) {
        a20_object_release(entry.object, entry.type);
        return -A20_ERR_ACCESS;
    }
    result = a20_handle_install_temporal(dest, entry.object, entry.type,
        requested ? requested : entry.rights, entry.expiry_tick,
        entry.remaining_ops, entry.temporal_flags, entry.security_label);
    if (result < 0) a20_object_release(entry.object, entry.type);
    return result;
}

int64_t sys_a20_task_clone(const a20_syscall_args_t *args)
{
    a20_clone_args_t *uargs = (void *)(uintptr_t)A20_ARG(0);
    a20_clone_args_t kargs;
    A20_VALIDATE_AND_COPY(uargs, kargs);
    if ((kargs.flags & ~(A20_CLONE_COW_VM | A20_CLONE_STACK)) ||
        kargs.reserved || kargs.reserved1 ||
        kargs.handle_count > A20_CLONE_MANIFEST_MAX ||
        ((kargs.flags & A20_CLONE_STACK) && (!kargs.stack || (kargs.stack & 15))))
        return -A20_ERR_INVALID_ARGUMENT;
    struct a20_ht_internal *ht = task_get_a20_ht(proc_current());
    if (!ht) return -A20_ERR_BAD_HANDLE;
    a20_clone_handle_t *manifest = NULL;
    struct a20_ht_internal *new_ht = NULL;
    task_t *new_task = NULL;
    a20_handle_t task_h = A20_HANDLE_NULL;
    int64_t result = -A20_ERR_NO_MEMORY;
    if (kargs.handle_count) {
        size_t bytes = kargs.handle_count * sizeof(*manifest);
        manifest = kmalloc(bytes);
        if (!manifest) goto out;
        if (copy_from_user(manifest, (void *)(uintptr_t)kargs.handles, bytes) < 0) {
            result = -A20_ERR_FAULT;
            goto out;
        }
    }
    new_ht = a20_ht_create();
    if (!new_ht) goto out;
    a20_ht_set_label(new_ht, a20_ht_get_label(ht));
    a20_handle_t child_root = A20_HANDLE_NULL, child_cwd = A20_HANDLE_NULL;
    if (kargs.root_dir != A20_HANDLE_NULL) {
        result = a20_clone_install(ht, new_ht, kargs.root_dir, A20_OBJ_DIRECTORY, 0);
        if (result < 0) goto out;
        child_root = (a20_handle_t)result;
    }
    if (kargs.cwd_dir != A20_HANDLE_NULL) {
        result = a20_clone_install(ht, new_ht, kargs.cwd_dir, A20_OBJ_DIRECTORY, 0);
        if (result < 0) goto out;
        child_cwd = (a20_handle_t)result;
    } else child_cwd = child_root;
    for (uint32_t i = 0; i < kargs.handle_count; i++) {
        result = a20_clone_install(ht, new_ht, manifest[i].parent_handle,
                                   A20_OBJ_INVALID, manifest[i].child_rights);
        if (result < 0) goto out;
        manifest[i].child_handle = (a20_handle_t)result;
    }

    /* Validate and acquire every declared capability before creating a child.
     * No failed transfer may leave a partly initialized runnable process. */
    int child_pid = proc_clone_deferred(
        (kargs.flags & A20_CLONE_STACK) ? (vaddr_t)kargs.stack : 0, &new_task);
    if (child_pid < 0 || !new_task) {
        result = child_pid == -EAGAIN ? -A20_ERR_WOULD_BLOCK : -A20_ERR_NO_MEMORY;
        goto out;
    }
    new_task->abi_mode = 1;
    __atomic_store_n(&new_task->a20_ht, new_ht, __ATOMIC_RELEASE);
    new_ht = NULL; /* now owned by the task */
    struct a20_ht_internal *child_ht = task_get_a20_ht(new_task);
    int64_t self_h = a20_handle_install_task(child_ht, new_task, A20_OBJ_TASK,
        A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT | A20_RIGHT_CONTROL |
        A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
    if (self_h < 0) { result = self_h; goto out; }
    new_task->a20_self_h = (a20_handle_t)self_h;
    result = a20_handle_install_task(ht, new_task, A20_OBJ_TASK,
        A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT | A20_RIGHT_CONTROL |
        A20_RIGHT_DUP | A20_RIGHT_TRANSFER | A20_RIGHT_ADMIN);
    if (result < 0) goto out;
    task_h = (a20_handle_t)result;
    a20_clone_args_t child_args = kargs;
    child_args.out_task = A20_HANDLE_NULL;
    child_args.out_root = child_root;
    child_args.out_cwd = child_cwd;
    child_args.out_self = (a20_handle_t)self_h;
    if ((kargs.handle_count && proc_copy_to_task_user(new_task,
            (void *)(uintptr_t)kargs.handles, manifest,
            kargs.handle_count * sizeof(*manifest)) < 0) ||
        proc_copy_to_task_user(new_task, uargs, &child_args, sizeof(child_args)) < 0) {
        result = -A20_ERR_FAULT;
        goto out;
    }
    kargs.out_task = task_h;
    kargs.out_root = kargs.out_cwd = kargs.out_self = A20_HANDLE_NULL;
    if (a20_copy_struct_to_user(uargs, &kargs, sizeof(kargs)) < 0) {
        result = -A20_ERR_FAULT;
        goto out;
    }
    kfree(manifest);
    proc_make_ready(new_task);
    return child_pid;
out:
    if (task_h != A20_HANDLE_NULL) a20_handle_remove(ht, task_h);
    /* Deferred tasks have not run. Use the construction rollback path, which
     * unlinks the task and drops its MM, handle table and identity references. */
    if (new_task) proc_destroy_task(new_task);
    if (new_ht) a20_ht_put_ref(new_ht);
    kfree(manifest);
    return result;
}

/* Declare the process's current native standard handles (fd 0/1/2, root,
 * cwd, self).  mlibc's fork() child calls this after rebuilding its fd table
 * so that a subsequent exec can preserve them (pipeline read/write ends). */
int64_t sys_a20_task_adopt(const a20_syscall_args_t *args)
{
    a20_handle_t in[6];
    if (copy_from_user(in, (void *)(uintptr_t)A20_ARG(0), sizeof(in)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    for (int i = 0; i < 6; i++) {
        if (in[i] == A20_HANDLE_NULL)
            continue;
        a20_handle_entry_t e;
        if (a20_handle_lookup_ref_internal(ht, in[i], A20_OBJ_INVALID, 0, &e) < 0)
            return -A20_ERR_BAD_HANDLE;
        a20_object_release(e.object, e.type);
    }

    cur->a20_stdin_h = in[0];
    cur->a20_stdout_h = in[1];
    cur->a20_stderr_h = in[2];
    cur->a20_root_h = in[3];
    cur->a20_cwd_h = in[4];
    cur->a20_self_h = in[5];
    cur->a20_stdio_adopted = 1;
    return A20_OK;
}

/* ================================================================== */
/*  execve — in-place image replacement (fork's partner in the child)  */
/* ================================================================== */

extern int64_t a20_native_vfs_result(int r);

int64_t sys_a20_execve(const a20_syscall_args_t *args)
{
    uint64_t path_user = A20_ARG(0), argv_user = A20_ARG(1), envp_user = A20_ARG(2);
    int extended = path_user == A20_EXEC_STRUCT;
    a20_exec_bootstrap_args_t kargs = {0};
    if (extended) {
        a20_exec_bootstrap_args_t *uargs = (void *)(uintptr_t)A20_ARG(1);
        A20_VALIDATE_AND_COPY(uargs, kargs);
        path_user = kargs.path; argv_user = kargs.argv; envp_user = kargs.envp;
    }
    char path[MAX_PATH_LEN];
    long error = user_path_strncpy(path, (const char *)(uintptr_t)path_user, sizeof(path));
    if (error < 0) return a20_native_vfs_result((int)error);
    proc_exec_bootstrap_t bootstrap = {0};
    void *data = NULL;
    uint32_t *close_handles = NULL;
    if (extended) {
        if (kargs.reserved || kargs.data_size > A20_EXEC_BOOTSTRAP_MAX ||
            kargs.close_count > A20_EXEC_CLOSE_MAX)
            return -A20_ERR_INVALID_ARGUMENT;
        if (kargs.data_size) {
            data = kmalloc(kargs.data_size);
            if (!data) return -A20_ERR_NO_MEMORY;
            if (copy_from_user(data, (void *)(uintptr_t)kargs.data, kargs.data_size) < 0) {
                kfree(data);
                return -A20_ERR_FAULT;
            }
        }
        if (kargs.close_count) {
            size_t size = kargs.close_count * sizeof(uint32_t);
            close_handles = kmalloc(size);
            if (!close_handles) {
                kfree(data);
                return -A20_ERR_NO_MEMORY;
            }
            if (copy_from_user(close_handles, (void *)(uintptr_t)kargs.close_handles, size) < 0) {
                kfree(close_handles);
                kfree(data);
                return -A20_ERR_FAULT;
            }
        }
        bootstrap.data = data;
        bootstrap.size = kargs.data_size;
        bootstrap.close_handles = close_handles;
        bootstrap.close_count = kargs.close_count;
    }
    int r = proc_exec_with_bootstrap(path, (char **)(uintptr_t)argv_user,
                                     (char **)(uintptr_t)envp_user, &bootstrap);
    kfree(close_handles);
    kfree(data);
    return a20_native_vfs_result(r);
}

/* ===== Time (0x0700) ===== */

int64_t sys_a20_clock_get(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)A20_ARG(0);
    a20_time_ns_t *out = (a20_time_ns_t *)(uintptr_t)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    uint64_t ts[2]; /* [seconds, nanoseconds] */
    switch (clock_id) {
    case 0: /* CLOCK_REALTIME */
    case 5: /* CLOCK_REALTIME_COARSE */
        timekeeping_get_realtime(ts);
        break;
    case 1: /* CLOCK_MONOTONIC */
    case 4: /* CLOCK_MONOTONIC_RAW */
    case 6: /* CLOCK_MONOTONIC_COARSE */
        timekeeping_get_monotonic(ts);
        break;
    default:
        return -A20_ERR_INVALID_ARGUMENT;
    }

    a20_time_ns_t ns = ts[0] * 1000000000ULL + ts[1];
    if (copy_to_user(out, &ns, sizeof(ns)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

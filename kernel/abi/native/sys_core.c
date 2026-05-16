/*
 * A20OS Native ABI — Phase 1 syscall implementations.
 *
 * Implemented with real kernel API backing:
 *   abi_info, feature_test,
 *   handle_close/dup/query/replace/close_many/seek,
 *   task_exit, task_wait,
 *   vm_alloc, vm_unmap,
 *   path_open, handle_read, handle_write, handle_stat,
 *   clock_get.
 *
 * Still stub (returns NOT_SUPPORTED): task_spawn, and all Phase 2+ syscalls.
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
#include "core/stdio.h"
#include "core/timekeeping.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "mm/mm.h"
#include "mm/elf.h"
#include "fs/vfs.h"
#include "fs/fdtable.h"
#include "sys/usercopy.h"

#include "abi/native/types.h"
#include "abi/native/errno.h"
#include "abi/native/rights.h"
#include "abi/native/syscall_entry.h"
#include "abi/native/startup.h"

#define A20_ARG(n) (args->arg[(n)])

/* Internal handle table functions (from handle_table.c) */
struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                            uint16_t type, a20_rights_t rights);
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                    uint16_t type, a20_rights_t rights,
                                    uint64_t expiry_tick, uint32_t remaining_ops,
                                    uint32_t temporal_flags, uint8_t security_label);
int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                    uint16_t expected_type, a20_rights_t required_rights,
                                    a20_handle_entry_t *out);
void a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
struct a20_ht_internal *task_get_a20_ht(task_t *t);
uint8_t a20_ht_get_label(struct a20_ht_internal *ht);

/* ===== Core (0x0000) ===== */

int64_t sys_a20_abi_info(const a20_syscall_args_t *args)
{
    a20_abi_info_t *out = (a20_abi_info_t *)A20_ARG(0);
    if (!out) return -A20_ERR_FAULT;

    a20_abi_info_t info;
    memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    info.version = 1;
    info.abi_major = A20_ABI_MAJOR;
    info.abi_minor = A20_ABI_MINOR;
    info.abi_patch = A20_ABI_PATCH;
    info.pointer_bits = 64;
    info.page_size = 4096;
    info.handle_bits = 32;
    /* feature_bits: bit 0 = handle_table, bit 1 = temporal_capabilities */
    info.feature_bits[0] = (1ULL << 0) | (1ULL << 1);
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
    features[0] = (1ULL << 0) | (1ULL << 1); /* handle_table + temporal */

    if (features[word] & (1ULL << bit))
        return A20_OK;
    return -A20_ERR_NOT_SUPPORTED;
}

/* ===== Handle (0x0100) ===== */

int64_t sys_a20_handle_close(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* Lookup without requiring any specific rights — closing always works
     * on valid handles. We read the entry to potentially release object refs. */
    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID, 0, &entry);
    if (ret < 0) return ret;

    /* For vfile-backed objects, close the underlying kernel fd.
     * The object pointer in handle entries for file/dir/device/pipe types
     * stores the kernel global fd as an integer (cast to void*). */
    if (entry.object != NULL &&
        (entry.type == A20_OBJ_FILE || entry.type == A20_OBJ_DIRECTORY ||
         entry.type == A20_OBJ_PIPE_ENDPOINT || entry.type == A20_OBJ_DEVICE)) {
        int gfd = (int)(uintptr_t)entry.object;
        vfs_close(gfd);
    }

    a20_handle_remove(ht, h);
    return A20_OK;
}

int64_t sys_a20_handle_dup(const a20_syscall_args_t *args)
{
    a20_handle_dup_args_t *uargs = (a20_handle_dup_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_handle_dup_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t src;
    int64_t ret = a20_handle_lookup_internal(ht, kargs.source,
                                              A20_OBJ_INVALID, A20_RIGHT_DUP, &src);
    if (ret < 0) return ret;

    /* rights_mask must be subset of source rights docs/native-abi/06-security.md §2.2) */
    if (kargs.rights_mask != 0 && (kargs.rights_mask & ~src.rights) != 0)
        return -A20_ERR_ACCESS;

    a20_rights_t new_rights = kargs.rights_mask ? kargs.rights_mask : src.rights;
    int64_t new_h = a20_handle_install_temporal(ht, src.object, src.type, new_rights,
                                                src.expiry_tick, src.remaining_ops,
                                                src.temporal_flags, src.security_label);
    if (new_h < 0) return new_h;

    /* For vfile-backed objects, dup the kernel fd ref */
    if (src.object != NULL &&
        (src.type == A20_OBJ_FILE || src.type == A20_OBJ_DIRECTORY ||
         src.type == A20_OBJ_PIPE_ENDPOINT || src.type == A20_OBJ_DEVICE)) {
        int gfd = (int)(uintptr_t)src.object;
        vfs_ref_fd(gfd);
    }

    kargs.out_handle = (a20_handle_t)new_h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_handle_query(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_handle_info_t *out = (a20_handle_info_t *)A20_ARG(1);
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
    a20_handle_t *out = (a20_handle_t *)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t src;
    int64_t ret = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                              A20_RIGHT_DUP, &src);
    if (ret < 0) return ret;

    if (rights != 0 && (rights & ~src.rights) != 0)
        return -A20_ERR_ACCESS;

    a20_rights_t new_rights = rights ? rights : src.rights;
    int64_t new_h = a20_handle_install_temporal(ht, src.object, src.type, new_rights,
                                                src.expiry_tick, src.remaining_ops,
                                                src.temporal_flags, src.security_label);
    if (new_h < 0) return new_h;

    /* Atomic replace: remove old, keep refcount same docs/native-abi/03-handle.md §4.3) */
    a20_handle_remove(ht, h);

    if (out) {
        a20_handle_t result = (a20_handle_t)new_h;
        if (copy_to_user(out, &result, sizeof(result)) < 0)
            return -A20_ERR_FAULT;
    }
    return A20_OK;
}

int64_t sys_a20_handle_close_many(const a20_syscall_args_t *args)
{
    uint64_t handles_ptr = A20_ARG(0);
    uint32_t count = (uint32_t)A20_ARG(1);
    a20_handle_t *handles = (a20_handle_t *)handles_ptr;
    if (!handles || count > 4096) return -A20_ERR_FAULT;

    uint32_t closed = 0;
    for (uint32_t i = 0; i < count; i++) {
        a20_syscall_args_t single = { .arg = { handles[i] } };
        if (sys_a20_handle_close(&single) >= 0)
            closed++;
    }
    return (int64_t)closed;
}

int64_t sys_a20_handle_seek(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_off_t *offset_ptr = (a20_off_t *)A20_ARG(1);
    uint32_t whence = (uint32_t)A20_ARG(2);

    if (!offset_ptr) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                              A20_RIGHT_SEEK, &entry);
    if (ret < 0) return ret;

    /* Only file-type handles support seeking */
    if (entry.type != A20_OBJ_FILE && entry.type != A20_OBJ_DEVICE)
        return -A20_ERR_INVALID_ARGUMENT;

    a20_off_t cur_offset;
    if (copy_from_user(&cur_offset, offset_ptr, sizeof(cur_offset)) < 0)
        return -A20_ERR_FAULT;

    int gfd = (int)(uintptr_t)entry.object;
    long new_off = vfs_lseek(gfd, (long)cur_offset, (int)whence);
    if (new_off < 0) return -A20_ERR_INVALID_ARGUMENT;

    cur_offset = (a20_off_t)new_off;
    if (copy_to_user(offset_ptr, &cur_offset, sizeof(cur_offset)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

/* ===== Task (0x0200) ===== */

int64_t sys_a20_task_exit(const a20_syscall_args_t *args)
{
    int32_t code = (int32_t)A20_ARG(0);
    task_t *cur = proc_current();

    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (ht) {
        a20_ht_destroy(ht);
        cur->scratch_buf = NULL;
    }

    proc_exit(code);
    return 0;
}

int64_t sys_a20_task_spawn(const a20_syscall_args_t *args)
{
    a20_task_spawn_args_t *uargs = (a20_task_spawn_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_task_spawn_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t img_entry;
    int64_t r = a20_handle_lookup_internal(ht, kargs.image, A20_OBJ_FILE,
                                           A20_RIGHT_READ | A20_RIGHT_EXEC, &img_entry);
    if (r < 0) return r;

    int img_fd = (int)(uintptr_t)img_entry.object;
    vfile_t *img_vf = vfs_get_file_ref(img_fd);
    if (!img_vf) return -A20_ERR_BAD_HANDLE;

    char path_buf[MAX_PATH_LEN];
    strncpy(path_buf, img_vf->path, MAX_PATH_LEN);
    vfs_put_file_ref(img_fd, img_vf);

    elf_load_info_t info;
    memset(&info, 0, sizeof(info));
    r = elf_load(img_fd, path_buf, &info);
    if (r < 0) return -A20_ERR_IO;

    char *argv_buf[16] = {0};
    char *envp_buf[16] = {0};
    int argc = (int)kargs.argc;
    int envc = (int)kargs.envc;

    if (argc > 0 && kargs.argv) {
        int copy_n = argc < 16 ? argc : 16;
        if (copy_from_user(argv_buf, (void *)kargs.argv, copy_n * sizeof(char *)) < 0)
            return -A20_ERR_FAULT;
    }
    if (envc > 0 && kargs.envp) {
        int copy_n = envc < 16 ? envc : 16;
        if (copy_from_user(envp_buf, (void *)kargs.envp, copy_n * sizeof(char *)) < 0)
            return -A20_ERR_FAULT;
    }

    size_t total_vm = (size_t)(info.end_va - info.base);
    int new_pid = proc_alloc_user_image(info.entry, info.stack_top, info.pgdir,
                                         info.mmap, info.brk,
                                         info.stack_top, total_vm);
    if (new_pid < 0) return -A20_ERR_NO_MEMORY;

    task_t *new_task = proc_find(new_pid);
    if (!new_task) return -A20_ERR_NO_MEMORY;

    new_task->abi_mode = 1;

    struct a20_ht_internal *new_ht = a20_ht_create();
    if (!new_ht) { proc_force_exit(new_task, 1); return -A20_ERR_NO_MEMORY; }
    new_task->scratch_buf = new_ht;

    a20_handle_entry_t root_entry;
    if (a20_handle_lookup_internal(ht, kargs.root_dir, A20_OBJ_DIRECTORY,
                                   A20_RIGHT_READ | A20_RIGHT_STAT, &root_entry) == A20_OK) {
        int fd = (int)(uintptr_t)root_entry.object;
        vfs_ref_fd(fd);
        a20_handle_install(new_ht, root_entry.object, A20_OBJ_DIRECTORY,
                           A20_RIGHT_READ | A20_RIGHT_STAT |
                           A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
    }

    /* ---- Transfer handle array from parent to child (spawn.md §3.4) ---- */
    if (kargs.handle_count > 0 && kargs.handles) {
        uint32_t nh = kargs.handle_count;
        if (nh > 64) nh = 64;
        a20_spawn_handle_t sh_buf[64];
        if (copy_from_user(sh_buf, (void *)kargs.handles, nh * sizeof(a20_spawn_handle_t)) < 0) {
            proc_force_exit(new_task, 1);
            return -A20_ERR_FAULT;
        }
        for (uint32_t i = 0; i < nh; i++) {
            a20_spawn_handle_t *sh = &sh_buf[i];
            /* Parent must have DUP right to transfer (rights.md §4.2) */
            a20_handle_entry_t src;
            int64_t lr = a20_handle_lookup_internal(ht, sh->handle, A20_OBJ_INVALID,
                                                     A20_RIGHT_DUP, &src);
            if (lr < 0) continue;

            /* Mask child rights to requested subset (never exceed source) */
            a20_rights_t child_rights = sh->rights & src.rights;
            if (child_rights == 0) continue;

            /* Ref-count the underlying object if it's a vfile */
            if (src.type == A20_OBJ_FILE || src.type == A20_OBJ_DIRECTORY) {
                int fd = (int)(uintptr_t)src.object;
                vfs_ref_fd(fd);
            }

            int64_t child_h = a20_handle_install(new_ht, src.object, src.type, child_rights);
            /* If target_slot specified, try to install at that exact slot */
            if (child_h >= 0 && sh->target_slot != 0 &&
                sh->target_slot != (uint32_t)child_h) {
                /* Best-effort: we installed at child_h, target_slot is advisory */
                /* Future: slot-swap support. For now, accept what alloc gave us. */
                (void)sh->target_slot;
            }
        }
    }

    int64_t child_self_h = a20_handle_install(new_ht, (void *)(uintptr_t)new_pid, A20_OBJ_TASK,
                       A20_RIGHT_WAIT | A20_RIGHT_SIGNAL | A20_RIGHT_STAT |
                       A20_RIGHT_DUP | A20_RIGHT_TRANSFER);

    int64_t task_h = a20_handle_install(ht, (void *)(uintptr_t)new_pid,
                                         A20_OBJ_TASK,
                                         A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                                         A20_RIGHT_STAT | A20_RIGHT_DUP |
                                         A20_RIGHT_TRANSFER);
    if (task_h < 0) {
        proc_force_exit(new_task, 1);
        return task_h;
    }

    kargs.out_task = (a20_handle_t)task_h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    a20_handle_t child_self_task = (child_self_h >= 0) ? (a20_handle_t)child_self_h : 0;

    uint64_t sp = elf_setup_stack_a20(info.stack_top, argc, argv_buf, envp_buf,
                                      &info, 0, 0, 0, child_self_task);
    if (sp == 0) { proc_force_exit(new_task, 1); return -A20_ERR_NO_MEMORY; }

    trap_context_t *trap = (trap_context_t *)
        ((uint64_t)new_task->kstack_base + KERNEL_STACK_SIZE - sizeof(trap_context_t));
    TRAP_CTX_SP(trap) = sp;
    TRAP_CTX_SET_ARG0(trap, sp);

    proc_make_ready(new_task);
    return task_h;
}

int64_t sys_a20_task_wait(const a20_syscall_args_t *args)
{
    a20_handle_t task_h = (a20_handle_t)A20_ARG(0);
    a20_flags_t flags = (a20_flags_t)A20_ARG(1);
    a20_task_status_t *out = (a20_task_status_t *)A20_ARG(2);

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    /* Lookup task handle with WAIT right docs/native-abi/06-security.md §3.2) */
    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_internal(ht, task_h, A20_OBJ_TASK,
                                              A20_RIGHT_WAIT, &entry);
    if (ret < 0) return ret;

    /* The object pointer for A20_OBJ_TASK is the task_t* itself */
    task_t *target = (task_t *)entry.object;
    if (!target) return -A20_ERR_BAD_HANDLE;

    int status = 0;
    int wret = proc_wait4(target->pid, &status, 0);
    if (wret < 0) return -A20_ERR_INVALID_ARGUMENT;

    if (out) {
        a20_task_status_t ts;
        memset(&ts, 0, sizeof(ts));
        ts.size = sizeof(ts);
        ts.version = 1;
        ts.exit_code = (status >> 8) & 0xFF;
        ts.exit_reason = 0;
        if (copy_to_user(out, &ts, sizeof(ts)) < 0)
            return -A20_ERR_FAULT;
    }
    return A20_OK;
}

/* ===== Memory (0x0300) ===== */

int64_t sys_a20_vm_alloc(const a20_syscall_args_t *args)
{
    a20_vm_alloc_args_t *uargs = (a20_vm_alloc_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_vm_alloc_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    if (kargs.length == 0) return -A20_ERR_INVALID_ARGUMENT;

    uint64_t addr = proc_mmap(kargs.addr_hint, (size_t)kargs.length,
                              (int)kargs.prot, 0x20 /* MAP_ANONYMOUS */,
                              -1, 0);
    if (addr == 0) return -A20_ERR_NO_MEMORY;

    kargs.out_addr = addr;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_vm_unmap(const a20_syscall_args_t *args)
{
    uint64_t addr = A20_ARG(0);
    uint64_t len  = A20_ARG(1);
    if (len == 0) return -A20_ERR_INVALID_ARGUMENT;
    return proc_munmap(addr, (size_t)len);
}

/* ===== Path / Filesystem (0x0400) ===== */

int64_t sys_a20_path_open(const a20_syscall_args_t *args)
{
    a20_path_open_args_t *uargs = (a20_path_open_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_path_open_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    /* Get path string from user */
    char kpath[MAX_PATH_LEN];
    const char *upath = (const char *)kargs.path;
    if (!upath) return -A20_ERR_FAULT;
    if (kargs.path_len > 0 && kargs.path_len < MAX_PATH_LEN) {
        if (copy_from_user(kpath, upath, kargs.path_len) < 0)
            return -A20_ERR_FAULT;
        kpath[kargs.path_len] = '\0';
    } else {
        /* nul-terminated — copy up to MAX_PATH_LEN */
        int i;
        for (i = 0; i < MAX_PATH_LEN - 1; i++) {
            if (copy_from_user(&kpath[i], &upath[i], 1) < 0)
                return -A20_ERR_FAULT;
            if (kpath[i] == '\0') break;
        }
        kpath[MAX_PATH_LEN - 1] = '\0';
    }

    /* Resolve path relative to current task's cwd */
    char full[MAX_PATH_LEN];
    task_t *cur = proc_current();
    if (kpath[0] == '/') {
        strncpy(full, kpath, MAX_PATH_LEN);
    } else {
        size_t cwd_len = strlen(cur->fs.cwd);
        if (cwd_len + 1 + strlen(kpath) >= MAX_PATH_LEN)
            return -A20_ERR_INVALID_ARGUMENT;
        memcpy(full, cur->fs.cwd, cwd_len);
        full[cwd_len] = '/';
        strncpy(full + cwd_len + 1, kpath, MAX_PATH_LEN - cwd_len - 1);
    }

    /* Open via VFS */
    int gfd = vfs_open(full, (int)kargs.flags, (int)kargs.mode);
    if (gfd < 0) return -A20_ERR_NOT_FOUND;

    /* Determine object type from vnode */
    uint16_t obj_type = A20_OBJ_FILE;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (vf && vf->vnode) {
        if (vf->vnode->type == VFS_FT_DIR)
            obj_type = A20_OBJ_DIRECTORY;
    }
    if (vf) vfs_put_file_ref(gfd, vf);

    /* Derive default rights from open flags docs/native-abi/06-security.md §3.2) */
    a20_rights_t rights = A20_RIGHT_STAT | A20_RIGHT_SEEK | A20_RIGHT_DUP |
                          A20_RIGHT_TRANSFER;
    if (!(kargs.flags & 0x1)) /* not O_WRONLY */
        rights |= A20_RIGHT_READ;
    if (kargs.flags & (0x1 | 0x2)) /* O_WRONLY or O_RDWR */
        rights |= A20_RIGHT_WRITE;
    if (kargs.rights != 0)
        rights = kargs.rights & rights; /* user can only restrict */

    /* Install handle in task's handle table.
     * For vfile-backed objects, store the kernel gfd as the object pointer. */
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) {
        vfs_close(gfd);
        return -A20_ERR_BAD_HANDLE;
    }

    int64_t h = a20_handle_install(ht, (void *)(uintptr_t)gfd, obj_type, rights);
    if (h < 0) {
        vfs_close(gfd);
        return h;
    }

    kargs.out_handle = (a20_handle_t)h;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

int64_t sys_a20_handle_read(const a20_syscall_args_t *args)
{
    a20_io_args_t *uargs = (a20_io_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_io_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_internal(ht, kargs.handle,
                                              A20_OBJ_INVALID, A20_RIGHT_READ,
                                              &entry);
    if (ret < 0) return ret;

    /* Bell-LaPadula No Read Up (docs/native-abi/06-security.md §5.2) */
    if (a20_ht_get_label(ht) < entry.security_label)
        return -A20_ERR_ACCESS;
    int gfd = (int)(uintptr_t)entry.object;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf) return -A20_ERR_BAD_HANDLE;

    /* Read iov buffers */
    uint64_t total_read = 0;
    a20_iovec_t *iov = (a20_iovec_t *)kargs.iov;
    for (uint32_t i = 0; i < kargs.iov_count; i++) {
        a20_iovec_t v;
        if (copy_from_user(&v, &iov[i], sizeof(v)) < 0) {
            total_read = (uint64_t)-A20_ERR_FAULT;
            break;
        }
        if (v.len == 0) continue;

        char *buf = (char *)v.base;
        if (!buf) continue;

        char kbuf[512];
        uint64_t remaining = v.len;
        uint64_t done = 0;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(kbuf) ? sizeof(kbuf) : (size_t)remaining;
            int64_t n = vfs_read_file(vf, kbuf, chunk);
            if (n < 0) {
                if (total_read == 0) total_read = (uint64_t)-A20_ERR_IO;
                goto read_done;
            }
            if (copy_to_user(buf + done, kbuf, (size_t)n) < 0) {
                total_read = (uint64_t)-A20_ERR_FAULT;
                goto read_done;
            }
            done += (uint64_t)n;
            remaining -= (uint64_t)n;
            total_read += (uint64_t)n;
            if ((uint64_t)n < chunk) break;
        }
    }
read_done:

    vfs_put_file_ref(gfd, vf);

    kargs.out_count = total_read;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)total_read;
}

int64_t sys_a20_handle_write(const a20_syscall_args_t *args)
{
    a20_io_args_t *uargs = (a20_io_args_t *)A20_ARG(0);
    if (!uargs) return -A20_ERR_FAULT;

    a20_io_args_t kargs;
    if (copy_from_user(&kargs, uargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_internal(ht, kargs.handle,
                                              A20_OBJ_INVALID, A20_RIGHT_WRITE,
                                              &entry);
    if (ret < 0) return ret;

    /* Bell-LaPadula No Write Down (docs/native-abi/06-security.md §5.2) */
    if (a20_ht_get_label(ht) > entry.security_label)
        return -A20_ERR_ACCESS;

    int gfd = (int)(uintptr_t)entry.object;
    vfile_t *vf = vfs_get_file_ref(gfd);
    if (!vf) return -A20_ERR_BAD_HANDLE;

    uint64_t total_written = 0;
    a20_iovec_t *iov = (a20_iovec_t *)kargs.iov;
    for (uint32_t i = 0; i < kargs.iov_count; i++) {
        a20_iovec_t v;
        if (copy_from_user(&v, &iov[i], sizeof(v)) < 0) {
            total_written = (uint64_t)-A20_ERR_FAULT;
            break;
        }
        if (v.len == 0) continue;

        const char *buf = (const char *)v.base;
        if (!buf) continue;

        char kbuf[512];
        uint64_t remaining = v.len;
        uint64_t done = 0;
        while (remaining > 0) {
            size_t chunk = remaining > sizeof(kbuf) ? sizeof(kbuf) : (size_t)remaining;
            if (copy_from_user(kbuf, buf + done, chunk) < (long)chunk) {
                total_written = (uint64_t)-A20_ERR_FAULT;
                goto write_done;
            }
            int64_t n = vfs_write_file(vf, kbuf, chunk);
            if (n < 0) {
                if (total_written == 0) total_written = (uint64_t)-A20_ERR_IO;
                goto write_done;
            }
            done += (uint64_t)n;
            remaining -= (uint64_t)n;
            total_written += (uint64_t)n;
            if ((uint64_t)n < chunk) break;
        }
    }
write_done:

    vfs_put_file_ref(gfd, vf);

    kargs.out_count = total_written;
    if (copy_to_user(uargs, &kargs, sizeof(kargs)) < 0)
        return -A20_ERR_FAULT;
    return (int64_t)total_written;
}

int64_t sys_a20_handle_stat(const a20_syscall_args_t *args)
{
    a20_handle_t h = (a20_handle_t)A20_ARG(0);
    a20_stat_t *out = (a20_stat_t *)A20_ARG(1);
    if (!out) return -A20_ERR_FAULT;

    task_t *cur = proc_current();
    struct a20_ht_internal *ht = task_get_a20_ht(cur);
    if (!ht) return -A20_ERR_BAD_HANDLE;

    a20_handle_entry_t entry;
    int64_t ret = a20_handle_lookup_internal(ht, h, A20_OBJ_INVALID,
                                              A20_RIGHT_STAT, &entry);
    if (ret < 0) return ret;

    /* For vfile-backed handles, use vfs_fstat */
    if (entry.type == A20_OBJ_FILE || entry.type == A20_OBJ_DIRECTORY ||
        entry.type == A20_OBJ_PIPE_ENDPOINT || entry.type == A20_OBJ_DEVICE) {
        int gfd = (int)(uintptr_t)entry.object;
        kstat_t ks;
        int sr = vfs_fstat(gfd, &ks);
        if (sr < 0) return -A20_ERR_IO;

        a20_stat_t st;
        memset(&st, 0, sizeof(st));
        st.size = sizeof(st);
        st.version = 1;
        st.dev = ks.st_dev;
        st.ino = ks.st_ino;
        st.mode = ks.st_mode;
        st.nlink = ks.st_nlink;
        st.uid = ks.st_uid;
        st.gid = ks.st_gid;
        st.size_bytes = ks.st_size;
        st.blocks = ks.st_blocks;
        st.atime_ns = ks.st_atime * 1000000000ULL + ks.st_atime_nsec;
        st.mtime_ns = ks.st_mtime * 1000000000ULL + ks.st_mtime_nsec;
        st.ctime_ns = ks.st_ctime * 1000000000ULL + ks.st_ctime_nsec;

        if (copy_to_user(out, &st, sizeof(st)) < 0)
            return -A20_ERR_FAULT;
        return A20_OK;
    }

    /* Non-file handles: return minimal info */
    a20_stat_t st;
    memset(&st, 0, sizeof(st));
    st.size = sizeof(st);
    st.version = 1;
    if (copy_to_user(out, &st, sizeof(st)) < 0)
        return -A20_ERR_FAULT;
    return A20_OK;
}

/* ===== Time (0x0700) ===== */

int64_t sys_a20_clock_get(const a20_syscall_args_t *args)
{
    uint32_t clock_id = (uint32_t)A20_ARG(0);
    a20_time_ns_t *out = (a20_time_ns_t *)A20_ARG(1);
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

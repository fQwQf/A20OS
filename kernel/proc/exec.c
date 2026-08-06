/*
 * A20OS — Process execution (proc_exec)
 *
 * Replaces the current process image with a new program loaded from the
 * filesystem.  Supports:
 *   - ELF64 executables (static & dynamically linked)
 *   - Script files with shebang (#!) interpreters, iteratively resolved
 *   - Dual ABI: Linux (argc/argv/envp/auxv) and Native (a20_start_info_t)
 *
 * Architecture:
 *   proc_exec() copies user args once into an exec_bprm, then iterates
 *   up to EXEC_MAX_DEPTH times trying first ELF then shebang.  No
 *   recursion — bounded stack usage, single point of cleanup.
 *
 * Design inspired by Linux's do_execve / struct linux_binprm.
 */

#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "proc/signal.h"
#include "fs/fdtable.h"
#include "fs/vfs.h"
#include "fs/vfs/path.h"
#include "mm/elf.h"
#include "mm/mm.h"
#include "mm/vdso.h"
#include "mm/vm.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/trap.h"
#include "sys/usercopy.h"
#ifdef CONFIG_ABI_NATIVE
#include "ipc/ipc.h"
#include "ipc/start_info.h"

struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
void a20_ht_put_ref(struct a20_ht_internal *ht);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
#endif

/* ================================================================== */
/*  Constants                                                         */
/* ================================================================== */

#define EXEC_MAX_DEPTH  4       /* max shebang nesting (Linux: BINPRM_MAX_RECURSION) */
#define EXEC_RETRY      1       /* return code: try again with rewritten bprm */
#define EXEC_DONE       0       /* return code: ELF loaded, install it */

/* ================================================================== */
/*  exec_bprm — central state for an in-progress exec                 */
/* ================================================================== */

typedef struct {
    char   *path;                       /* current executable path (kalloc'd) */
    char   *args[MAX_ARG_STRINGS + 1];  /* kernel copies of argv strings */
    char   *envs[MAX_ARG_STRINGS + 1];  /* kernel copies of envp strings */
    int     argc;
    int     envc;
    int     depth;                      /* shebang nesting depth */
} exec_bprm_t;

/* ================================================================== */
/*  Helpers: credential application (unchanged logic)                  */
/* ================================================================== */

static void proc_apply_exec_creds(task_t *t, const kstat_t *st)
{
    if (!t || !st)
        return;

    int old_uid  = t->cred.uid;
    int old_euid = t->cred.euid;
    int old_suid = t->cred.suid;

    if (st->st_mode & S_ISUID) {
        t->cred.euid  = (int)st->st_uid;
        t->cred.suid  = t->cred.euid;
        t->cred.fsuid = t->cred.euid;
    }
    if (st->st_mode & S_ISGID) {
        t->cred.egid  = (int)st->st_gid;
        t->cred.sgid  = t->cred.egid;
        t->cred.fsgid = t->cred.egid;
    }

    int old_had_root = old_uid == 0 || old_euid == 0 || old_suid == 0;
    int new_has_root = t->cred.uid == 0 || t->cred.euid == 0 || t->cred.suid == 0;
    if (old_had_root && !new_has_root) {
        t->cred.cap_effective = 0;
        t->cred.cap_permitted = 0;
    } else if (t->cred.euid == 0) {
        t->cred.cap_permitted = t->cred.cap_bounding;
        t->cred.cap_effective = t->cred.cap_permitted;
    } else if (old_euid == 0) {
        t->cred.cap_effective = 0;
    }
}

/* ================================================================== */
/*  bprm lifecycle                                                    */
/* ================================================================== */

static void bprm_free_strings(exec_bprm_t *bprm)
{
    for (int i = 0; i < bprm->argc; i++) {
        kfree(bprm->args[i]);
        bprm->args[i] = NULL;
    }
    for (int i = 0; i < bprm->envc; i++) {
        kfree(bprm->envs[i]);
        bprm->envs[i] = NULL;
    }
    bprm->argc = 0;
    bprm->envc = 0;
}

static void bprm_free(exec_bprm_t *bprm)
{
    bprm_free_strings(bprm);
    if (bprm->path) {
        kfree(bprm->path);
        bprm->path = NULL;
    }
}

static int exec_replace_path(exec_bprm_t *bprm, const char *path)
{
    char *copy = kmalloc(strlen(path) + 1);
    if (!copy)
        return -ENOMEM;
    strcpy(copy, path);
    kfree(bprm->path);
    bprm->path = copy;
    return 0;
}

/* ================================================================== */
/*  exec_copy_args — copy argv OR envp from user into kernel arrays   */
/* ================================================================== */

/*
 * Copy an array of string pointers from user space into kernel-owned
 * copies.  proc_exec() is an ABI syscall boundary, so argv and envp always
 * retain user-pointer provenance even on identity-mapped architectures.
 *
 * @src        pointer to the user pointer array, or NULL
 * @out        pre-allocated array of char* (at least MAX_ARG_STRINGS+1)
 * @out_count  output: number of strings copied
 * @arg_bytes  in/out: running total of bytes (for limit check)
 * @max_bytes  maximum allowed total (stack / 4)
 *
 * Returns 0 on success, negative errno on failure.
 * On failure, any partially copied strings are freed.
 */
static int exec_copy_args(char *const *src, char **out, int *out_count,
                          size_t *arg_bytes, size_t max_bytes)
{
    task_t *t = proc_current();
    int count = 0;

    if (!src) {
        out[0] = NULL;
        *out_count = 0;
        return 0;
    }

    while (count < MAX_ARG_STRINGS) {
        char *ptr;
        if (copy_from_user(&ptr, &src[count], sizeof(char *)) < 0) {
            /* cleanup */
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -EFAULT;
        }
        if (!ptr)
            break;

        out[count] = kmalloc(MAX_ARG_STRLEN);
        if (!out[count]) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -ENOMEM;
        }

        size_t len;
        long copied = user_strncpy(out[count], ptr, MAX_ARG_STRLEN);
        if (copied < 0) {
            kfree(out[count]); out[count] = NULL;
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return (int)copied;
        }
        if ((size_t)copied >= MAX_ARG_STRLEN - 1) {
            kfree(out[count]); out[count] = NULL;
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -E2BIG;
        }
        len = (size_t)copied + 1;

        *arg_bytes += len;
        if (*arg_bytes > max_bytes ||
            *arg_bytes > (t ? t->limits.stack / 4 : MAX_ARG_BYTES)) {
            kfree(out[count]); out[count] = NULL;
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -E2BIG;
        }
        count++;
    }

    /* Check for overflow: if the array has more than MAX_ARG_STRINGS entries */
    if (count == MAX_ARG_STRINGS) {
        char *extra;
        if (copy_from_user(&extra, &src[count], sizeof(char *)) < 0)
            extra = NULL;
        if (extra) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -E2BIG;
        }
    }

    out[count] = NULL;
    *out_count = count;
    return 0;
}

/* ================================================================== */
/*  exec_open_and_check — open file, validate type & permissions      */
/* ================================================================== */

/*
 * Opens the executable, checks it's a regular file with execute
 * permission, and returns the fd.  Also fills @st_out if non-NULL.
 *
 * Returns fd >= 0 on success, negative errno on failure.
 */
static int exec_open_and_check(const char *path, kstat_t *st_out)
{
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    kstat_t st;
    int sr = vfs_stat(path, &st);
    if (sr == 0) {
        if ((st.st_mode & S_IFMT) == S_IFDIR) {
            vfs_close(fd);
            return -EACCES;
        }
        if ((st.st_mode & S_IFMT) != S_IFREG) {
            vfs_close(fd);
            return -EACCES;
        }
        if (st.st_mode & 0111) {
            int xr = vfs_faccessat2(AT_FDCWD, path, X_OK, AT_EACCESS);
            if (xr < 0) {
                vfs_close(fd);
                return xr;
            }
        } else {
            /* No execute bit — only allowed if it's a script (checked later) */
        }
        if (st_out)
            *st_out = st;
    }

    return fd;
}

/* ================================================================== */
/*  exec_try_script — detect shebang and rewrite bprm for interpreter */
/* ================================================================== */

/*
 * Checks if @fd is a script (starts with #!).  If so, reads the
 * interpreter path and optional argument, rewrites bprm->path and
 * bprm->args for the interpreter, and returns EXEC_RETRY.
 *
 * If not a script, returns 0 (caller should try ELF).
 * On error, returns negative errno.
 */
static int exec_try_script(int fd, exec_bprm_t *bprm)
{
    /* elf_load() probes the same open file description first and leaves its
     * offset after the ELF header.  Shebang recognition must always start at
     * byte zero; otherwise direct execve() of every script is misclassified
     * as a non-script and returns ENOEXEC. */
    int seek_result = vfs_lseek(fd, 0, SEEK_SET);
    if (seek_result < 0)
        return seek_result;

    char magic[2];
    int n = vfs_read(fd, magic, sizeof(magic));
    if (n < 0)
        return n;
    if (n < 2 || magic[0] != '#' || magic[1] != '!')
        return 0;   /* not a script, caller should try ELF */

    /* Read the full first line (up to 256 bytes) */
    char buf[256];
    seek_result = vfs_lseek(fd, 0, SEEK_SET);
    if (seek_result < 0)
        return seek_result;
    n = vfs_read(fd, buf, sizeof(buf) - 1);
    if (n < 0)
        return n;
    if (n < 2)
        return -ENOEXEC;

    buf[n] = '\0';

    /* Skip "#!" */
    char *cp = buf + 2;
    while (*cp == ' ' || *cp == '\t')
        ++cp;
    if (*cp == '\0' || *cp == '\n' || *cp == '\r')
        return -ENOEXEC;

    /* Extract interpreter path */
    char *start = cp;
    while (*cp && *cp != '\n' && *cp != '\r' && *cp != ' ' && *cp != '\t')
        ++cp;
    size_t ilen = (size_t)(cp - start);
    if (ilen == 0 || ilen >= MAX_PATH_LEN)
        return -ENOEXEC;

    char interp_path[MAX_PATH_LEN];
    memcpy(interp_path, start, ilen);
    interp_path[ilen] = '\0';

    /* Extract optional single argument */
    char interp_arg[MAX_PATH_LEN];
    interp_arg[0] = '\0';
    while (*cp == ' ' || *cp == '\t')
        ++cp;
    if (*cp && *cp != '\n' && *cp != '\r') {
        char *arg_start = cp;
        while (*cp && *cp != '\n' && *cp != '\r')
            ++cp;
        size_t alen = (size_t)(cp - arg_start);
        if (alen > 0 && alen < MAX_PATH_LEN) {
            memcpy(interp_arg, arg_start, alen);
            interp_arg[alen] = '\0';
        }
    }

    /*
     * Build new argv:  interp_path [interp_arg] bprm->path [original args from index 1...]
     *
     * We reuse bprm->args[] in place.  Strategy:
     *   1. The original bprm->path is the script's path (becomes argv[1] or argv[2]).
     *   2. Free the old strings that won't be used, shift the rest.
     */
    int new_argc = 0;
    char *new_args[MAX_ARG_STRINGS + 1];

    /* interp_path (argv[0]) */
    new_args[new_argc] = kmalloc(strlen(interp_path) + 1);
    if (!new_args[new_argc]) goto nomem;
    strcpy(new_args[new_argc], interp_path);
    new_argc++;

    /* optional interp_arg (argv[1]) */
    if (interp_arg[0]) {
        new_args[new_argc] = kmalloc(strlen(interp_arg) + 1);
        if (!new_args[new_argc]) goto nomem;
        strcpy(new_args[new_argc], interp_arg);
        new_argc++;
    }

    /* script path (the original bprm->path) */
    new_args[new_argc] = kmalloc(strlen(bprm->path) + 1);
    if (!new_args[new_argc]) goto nomem;
    strcpy(new_args[new_argc], bprm->path);
    new_argc++;

    /* original args from index 1 onward (skip argv[0] which was the script) */
    for (int i = 1; i < bprm->argc && new_argc < MAX_ARG_STRINGS; i++) {
        new_args[new_argc++] = bprm->args[i];
        bprm->args[i] = NULL;   /* ownership transferred, don't free */
    }
    new_args[new_argc] = NULL;

    /* Free old argv[0] (the original script path in args) and old bprm->path */
    if (bprm->args[0]) { kfree(bprm->args[0]); bprm->args[0] = NULL; }
    if (bprm->path) { kfree(bprm->path); bprm->path = NULL; }

    /* Install new state */
    memcpy(bprm->args, new_args, (new_argc + 1) * sizeof(char *));
    bprm->argc = new_argc;

    /* Set new path to interpreter */
    bprm->path = kmalloc(strlen(interp_path) + 1);
    if (!bprm->path) goto nomem;
    strcpy(bprm->path, interp_path);

    return EXEC_RETRY;

nomem:
    for (int i = 0; i < new_argc; i++)
        kfree(new_args[i]);
    return -ENOMEM;
}

/* ================================================================== */
/*  Native ABI setup                                                  */
/* ================================================================== */

#ifdef CONFIG_ABI_NATIVE
/*
 * Set up handle table, console handles, and a20_start_info_t on the
 * stack for a native ABI process.  Returns the new stack pointer, or 0
 * on failure.
 */
static uint64_t exec_setup_native_abi(task_t *t,
                                       const elf_load_info_t *info,
                                       int argc, char *const *k_argv,
                                       char *const *k_envp,
                                       vaddr_t *start_info_out)
{
    struct a20_ht_internal *ht = a20_ht_create();
    if (ht) {
        /* Release the pre-exec table (if any) before installing the new one;
         * exec replaces the whole address space and capability set. */
        void *old = __atomic_exchange_n(&t->a20_ht, ht, __ATOMIC_ACQ_REL);
        if (old)
            a20_ht_put_ref((struct a20_ht_internal *)old);
    }

    uint32_t stdin_h = 0xFFFFFFFF, stdout_h = 0xFFFFFFFF, stderr_h = 0xFFFFFFFF;
    if (ht) {
        int console_rd = vfs_open("/dev/console", O_RDONLY, 0);
        if (console_rd >= 0) {
            int64_t h = a20_handle_install(ht, (void *)(uintptr_t)console_rd,
                                           A20_OBJ_FILE, A20_RIGHT_READ | A20_RIGHT_SEEK | A20_RIGHT_DUP);
            if (h >= 0) stdin_h = (uint32_t)h;
        }
        int console_wr = vfs_open("/dev/console", O_WRONLY, 0);
        if (console_wr >= 0) {
            int64_t h = a20_handle_install(ht, (void *)(uintptr_t)console_wr,
                                           A20_OBJ_FILE, A20_RIGHT_WRITE | A20_RIGHT_SEEK | A20_RIGHT_DUP);
            if (h >= 0) stdout_h = (uint32_t)h;
        }
        int console_wr2 = vfs_open("/dev/console", O_WRONLY, 0);
        if (console_wr2 >= 0) {
            int64_t h = a20_handle_install(ht, (void *)(uintptr_t)console_wr2,
                                           A20_OBJ_FILE, A20_RIGHT_WRITE | A20_RIGHT_SEEK | A20_RIGHT_DUP);
            if (h >= 0) stderr_h = (uint32_t)h;
        }
    }

    a20_handle_t self_h = 0;
    if (ht) {
        int64_t h = a20_handle_install(ht, (void *)(uintptr_t)t->pid,
                                       A20_OBJ_TASK,
                                       A20_RIGHT_WAIT | A20_RIGHT_SIGNAL |
                                       A20_RIGHT_STAT | A20_RIGHT_DUP);
        if (h >= 0) self_h = (a20_handle_t)h;
    }

    uint32_t root_h = 0, cwd_h = 0;
    if (ht) {
        int root_fd = vfs_open("/", O_RDONLY, 0);
        if (root_fd >= 0) {
            int64_t h = a20_handle_install(ht, (void *)(uintptr_t)root_fd,
                                           A20_OBJ_DIRECTORY,
                                           A20_RIGHT_READ | A20_RIGHT_STAT |
                                           A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
            if (h >= 0) {
                root_h = (uint32_t)h;
                vfs_ref_fd(root_fd);
                int64_t h2 = a20_handle_install(ht, (void *)(uintptr_t)root_fd,
                                                A20_OBJ_DIRECTORY,
                                                A20_RIGHT_READ | A20_RIGHT_STAT |
                                                A20_RIGHT_DUP);
                if (h2 >= 0) cwd_h = (uint32_t)h2;
                if (!cwd_h) cwd_h = root_h;
            }
        }
    }

    /* Well-known service registry client endpoint (M3). */
    uint32_t registry_h = 0;
    if (ht) {
        extern int64_t a20_registry_install_client(struct a20_ht_internal *ht);
        int64_t h = a20_registry_install_client(ht);
        if (h >= 0) registry_h = (uint32_t)h;
    }

    if (info->interp_base) {
        return elf_setup_stack_a20_dynamic(
            info->stack_top, argc, k_argv, k_envp, info,
            stdin_h, stdout_h, stderr_h, self_h, root_h, cwd_h,
            0,
            start_info_out);
    }

    return elf_setup_stack_a20(info->stack_top, argc,
                               k_argv, k_envp, info,
                               stdin_h, stdout_h, stderr_h, self_h,
                               root_h, cwd_h, 0, registry_h);
}
#endif /* CONFIG_ABI_NATIVE */

/* ================================================================== */
/*  exec_install_process — swap mm, set up trap, signals, creds       */
/* ================================================================== */

/*
 * Atomically replaces the current process's address space and register
 * state with the newly loaded ELF image.  Handles both Linux and
 * Native ABI stack layouts.
 *
 * IMPORTANT: Prepares the new mm fully BEFORE detaching the old one,
 * so that failure during new-mm setup can be rolled back without
 * losing the old address space.
 *
 * @t       the current task
 * @info    ELF load result
 * @bprm    the exec parameters (args, path)
 * @abs_path resolved absolute path for t->exec_path
 * @st      stat result (may be NULL if stat failed)
 */
static int exec_install_process(task_t *t,
                                 const elf_load_info_t *info,
                                 const exec_bprm_t *bprm,
                                 const char *abs_path,
                                 const kstat_t *st)
{
    /* ---- 1. Build user stack (Linux or Native ABI) ---- */
    uint64_t sp;
    vaddr_t native_start_info = 0;
#ifdef CONFIG_ABI_NATIVE
    if (info->is_native_abi) {
        sp = exec_setup_native_abi(t, info, bprm->argc,
                                    (char *const *)bprm->args,
                                    (char *const *)bprm->envs,
                                    &native_start_info);
    } else
#endif
    {
        vaddr_t ehdr = vdso_auxv_ehdr();
        sp = elf_setup_stack(info->stack_top, bprm->argc,
                              (char *const *)bprm->args,
                              (char *const *)bprm->envs, info, ehdr);
#ifdef CONFIG_ABI_NATIVE
        /* Exec from a Native program into a Linux ABI program: the old
         * handle table is process-local and must be released here. */
        if (t->abi_mode == 1) {
            void *old = __atomic_exchange_n(&t->a20_ht, NULL,
                                            __ATOMIC_ACQ_REL);
            if (old)
                a20_ht_put_ref((struct a20_ht_internal *)old);
        }
#endif
        /* t->mm still points at the OLD address space here; the new one
         * exists only as info->pgdir/info->mmap until step 3.  Map the
         * vDSO into the image being built so AT_SYSINFO_EHDR is backed. */
        if (sp != 0 && ehdr != 0 &&
            vdso_map_image(info->pgdir, &info->mmap) < 0)
            return -ENOMEM;
    }
    if (sp == 0)
        return -ENOMEM;

#ifdef CONFIG_ABI_NATIVE
    t->abi_mode = info->is_native_abi ? 1 : 0;
#else
    t->abi_mode = 0;
#endif

    /* ---- 2. Create new mm BEFORE detaching old ---- */
    mm_struct_t *new_mm = kcalloc(1, sizeof(mm_struct_t));
    if (!new_mm)
        return -ENOMEM;

    new_mm->pgdir      = info->pgdir;
    new_mm->brk        = info->brk;
    new_mm->start_brk  = info->brk;
    new_mm->mmap_base  = MMAP_BASE_ADDR;
    new_mm->stack_top  = info->stack_top;
    new_mm->stack_bottom = info->stack_top - USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    new_mm->total_vm   = 0;
    new_mm->rss        = 0;
    spin_init(&new_mm->lock);
    spin_set_debug(&new_mm->lock, "mm", new_mm);
    refcount_set(&new_mm->refcount, 1);
    new_mm->mmap       = info->mmap;
    new_mm->has_vdso   = vdso_auxv_ehdr() != 0;

#ifdef CONFIG_NOMMU
    /* Transfer NOMMU segment allocation tracking into the new mm.
     * elf_load64/32 tracks allocations in a local mm, then moves them into
     * info->nommu_allocs[] via elf_transfer_nommu_allocs. We must copy them
     * into new_mm so vfork snapshot/restore knows what memory to save. */
    new_mm->num_nommu_allocs = info->num_nommu_allocs;
    for (int i = 0;
         i < info->num_nommu_allocs && i < NOMMU_ALLOC_MAX; i++) {
        new_mm->nommu_allocs[i] = info->nommu_allocs[i];
        new_mm->nommu_alloc_sizes[i] = info->nommu_alloc_sizes[i];
        new_mm->nommu_alloc_types[i] = info->nommu_alloc_types[i];
    }
#endif

    /* Map architecture-specific signal return trampoline page. */
    arch_setup_signal_trampoline(new_mm);

    /*
     * Linux exec is a thread-group operation.  A pthread shares its fdtable
     * with every sibling, so applying FD_CLOEXEC to that table would close
     * epoll/eventfd descriptors out from under still-running workers.  Make
     * the caller's table private first, request sibling exit while their old
     * mm/files references remain valid, and mutate only the private table.
     */
    int fd_err = fdtable_unshare(t);
    if (fd_err < 0) {
        kfree(new_mm);
        return fd_err;
    }
    proc_exec_terminate_siblings(t);
    fdtable_close_on_exec(t);

    /* ---- 3. Atomically swap mm ---- */
    uint64_t mm_swap_flags = spin_lock_irqsave(&proc_lock);
    mm_struct_t *old_mm    = t->mm;
    pt_root_t   *old_pgdir = t->pgdir;
    t->mm   = new_mm;
    t->pgdir = info->pgdir;
    spin_unlock_irqrestore(&proc_lock, mm_swap_flags);
    t->entry = info->entry;
    t->ustack = sp;
    strncpy(t->exec_path, abs_path, MAX_PATH_LEN - 1);
    t->exec_path[MAX_PATH_LEN - 1] = '\0';
    const char *base = strrchr(t->exec_path, '/');
    proc_set_name(t, base ? base + 1 : t->exec_path);

    /* ---- 4. Reset signal handlers (POSIX exec semantics) ---- */
    signal_exec_reset(t);

    /* ---- 5. Set up trap context for return to user ---- */
    uint64_t saved_kernel_sp = (uint64_t)(uintptr_t)t->kstack_base + KERNEL_STACK_SIZE;

    if (!t->trap_ctx) {
        uint64_t ks_top = saved_kernel_sp;
        trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
        /*
         * Ask the architecture where the initial task_context_t belongs.
         * x86_64 keeps it at the bottom of the kernel stack; the other arches
         * keep it just below the trap frame.
         */
        task_context_t *new_ctx  = arch_task_context_base(t->kstack_base, ks_top, trap);
        memset(new_ctx, 0, sizeof(*new_ctx));
        t->first_kernel_entry = (uintptr_t)user_trap_return;
        new_ctx->ra = (uint64_t)proc_task_first_entry;
        new_ctx->tp = (uint64_t)(uintptr_t)t;
        arch_task_context_set_user_tp(new_ctx, info->tls_tp);
        TASK_CTX_STATUS(new_ctx) = arch_task_user_resume_status();
        TASK_CTX_PAGE_TABLE(new_ctx) = arch_make_addr_space_token(info->pgdir);
        arch_task_context_set_initial_sp(new_ctx, trap, ks_top);
        t->trap_ctx = trap;
        t->kstack   = (uint64_t)new_ctx;
    }

    {
        trap_context_t *trap = t->trap_ctx;
        saved_kernel_sp = arch_trap_ctx_get_kernel_stack(trap, (uint64_t)(uintptr_t)trap);
        memset(trap, 0, sizeof(*trap));
        TRAP_CTX_KScratch0(trap) = arch_make_addr_space_token(info->pgdir);
        arch_trap_ctx_set_user_entry(trap, info->entry);
        TRAP_CTX_SP(trap)        = sp;
        TRAP_CTX_TP(trap)        = info->tls_tp;
#ifdef CONFIG_ABI_NATIVE
        if (info->is_native_abi) {
            TRAP_CTX_SET_ARG0(trap, native_start_info ? native_start_info : sp);
        }
#endif
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        arch_trap_ctx_set_kernel_stack(trap, saved_kernel_sp);
        TRAP_CTX_STATUS(trap) = arch_user_initial_status();
    }

    /* ---- 6. Apply SUID/SGID credentials ---- */
    t->exec_load_addr = info->load_addr;
    t->exec_load_size = info->load_size;
    if (st)
        proc_apply_exec_creds(t, st);

    /* ---- 7. Switch page tables, destroy old address space ---- */
    arch_write_addr_space_token(arch_make_addr_space_token(info->pgdir));
    arch_tlb_flush();

    if (old_mm) {
        mm_destroy(old_mm);
    } else if (old_pgdir && old_pgdir != proc_kernel_pgdir_shared()) {
        pt_destroy_user(old_pgdir);
    }

    proc_complete_vfork(t);

#ifdef CONFIG_NOMMU
    arch_flush_icache_range((const void *)info->load_addr, info->load_size);
#else
    arch_fence_i();
#endif
    return 0;
}

/* ================================================================== */
/*  proc_exec — public API                                            */
/* ================================================================== */

int proc_exec(const char *path, char *const argv[], char *const envp[])
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;

    /* ---- 1. Initialise bprm and copy args from user ONCE ---- */
    exec_bprm_t bprm;
    memset(&bprm, 0, sizeof(bprm));

    bprm.path = kmalloc(strlen(path) + 1);
    if (!bprm.path)
        return -ENOMEM;
    strcpy(bprm.path, path);

    size_t arg_bytes = 0;
    int r = exec_copy_args(argv, bprm.args, &bprm.argc,
                           &arg_bytes, MAX_ARG_BYTES);
    if (r < 0) {
        kfree(bprm.path);
        return r;
    }
    /*
     * Linux accepts execve(path, NULL, envp), and also an argv array whose
     * first entry is NULL, by supplying an empty argv[0].  Modern glibc
     * rejects a literal argc == 0 during startup, so normalize the call here
     * before constructing the initial stack.  Use the requested path as the
     * useful argv[0] value while preserving the Linux guarantee argc >= 1.
     */
    if (bprm.argc == 0) {
        size_t len = strlen(bprm.path) + 1;
        if (arg_bytes + len > MAX_ARG_BYTES ||
            arg_bytes + len > (t ? t->limits.stack / 4 : MAX_ARG_BYTES)) {
            bprm_free(&bprm);
            return -E2BIG;
        }
        bprm.args[0] = kmalloc(len);
        if (!bprm.args[0]) {
            bprm_free(&bprm);
            return -ENOMEM;
        }
        memcpy(bprm.args[0], bprm.path, len);
        bprm.args[1] = NULL;
        bprm.argc = 1;
        arg_bytes += len;
    }

    r = exec_copy_args(envp, bprm.envs, &bprm.envc,
                       &arg_bytes, MAX_ARG_BYTES);
    if (r < 0) {
        bprm_free(&bprm);
        return r;
    }

    /* ---- 2. Resolve absolute path for exec_path ---- */
    char abs_path[MAX_PATH_LEN];
    const char *cwd = t->fs.cwd[0] ? t->fs.cwd : "/";
    const char *root = t->fs.root_path[0] ? t->fs.root_path : "/";
    if (bprm.path[0] == '/') {
        if (strcmp(root, "/") == 0) {
            strncpy(abs_path, bprm.path, MAX_PATH_LEN - 1);
            abs_path[MAX_PATH_LEN - 1] = '\0';
        } else {
            snprintf(abs_path, MAX_PATH_LEN, "%s%s", root, bprm.path);
        }
    } else {
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(abs_path, MAX_PATH_LEN, "%s%s", cwd, bprm.path);
        else
            snprintf(abs_path, MAX_PATH_LEN, "%s/%s", cwd, bprm.path);
        if (strcmp(root, "/") != 0) {
            char rooted[MAX_PATH_LEN];
            snprintf(rooted, MAX_PATH_LEN, "%s%s", root, abs_path);
            strncpy(abs_path, rooted, MAX_PATH_LEN - 1);
            abs_path[MAX_PATH_LEN - 1] = '\0';
        }
    }
    vfs_path_normalize_absolute_with_root(abs_path, root);
    if (bprm.path[0] != '/') {
        r = exec_replace_path(&bprm, abs_path);
        if (r < 0) {
            bprm_free(&bprm);
            return r;
        }
    }

    /* ---- 3. Iterative format resolution loop ---- */
    kstat_t exec_st;
    int exec_stat_ok = 0;

    for (bprm.depth = 0; bprm.depth < EXEC_MAX_DEPTH; bprm.depth++) {
        /* Open and validate the file */
        int fd = exec_open_and_check(bprm.path, &exec_st);
        if (fd < 0) {
            bprm_free(&bprm);
            return fd;
        }

        /* Try ELF first */
        elf_load_info_t info;
        memset(&info, 0, sizeof(info));
        r = elf_load(fd, bprm.path, &info);

        if (r == 0) {
            /* ELF loaded successfully — install the new process image */
            vfs_close(fd);

            /* Check execute permission on the actual ELF binary.
             * Scripts are allowed to lack +x (the interpreter must have it),
             * but ELF binaries require it. */
            exec_stat_ok = (vfs_stat(bprm.path, &exec_st) == 0);
            if (exec_stat_ok == 0) {
                memset(&exec_st, 0, sizeof(exec_st));
            }

            r = exec_install_process(t, &info, &bprm, abs_path,
                                      exec_stat_ok ? &exec_st : NULL);
            bprm_free_strings(&bprm);
            kfree(bprm.path);
            return r;
        }

        /* ELF failed — try shebang */
        if (r == -ENOEXEC) {
            int sr = exec_try_script(fd, &bprm);
            vfs_close(fd);
            if (sr == EXEC_RETRY)
                continue;   /* loop with new interpreter */
            if (sr < 0) {
                bprm_free(&bprm);
                return sr;
            }
            /* Not a script either → ENOEXEC */
            bprm_free(&bprm);
            return -ENOEXEC;
        }

        /* ELF failed with a real error (not format mismatch) */
        vfs_close(fd);
        bprm_free(&bprm);
        return r;
    }

    /* Too many shebang levels */
    bprm_free(&bprm);
    return -ELOOP;
}

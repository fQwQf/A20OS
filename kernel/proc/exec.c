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
#include "mm/elf.h"
#include "mm/mm.h"
#include "mm/vm.h"
#include "core/consts.h"
#include "core/klog.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/trap.h"
#include "sys/usercopy.h"
#ifdef CONFIG_ABI_NATIVE
#include "abi/native/startup.h"
#include "abi/native/rights.h"
#include "abi/native/types.h"

struct a20_ht_internal;
struct a20_ht_internal *a20_ht_create(void);
int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, uint32_t rights);
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

/* ================================================================== */
/*  exec_copy_args — copy argv OR envp from user into kernel arrays   */
/* ================================================================== */

/*
 * Copy an array of string pointers from user or kernel space into
 * kernel-owned copies.  Works for both argv and envp.
 *
 * @src        pointer to the user/kernel pointer array
 * @is_kernel  if true, src is in kernel space
 * @out        pre-allocated array of char* (at least MAX_ARG_STRINGS+1)
 * @out_count  output: number of strings copied
 * @arg_bytes  in/out: running total of bytes (for limit check)
 * @max_bytes  maximum allowed total (stack / 4)
 *
 * Returns 0 on success, negative errno on failure.
 * On failure, any partially copied strings are freed.
 */
static int exec_copy_args(char *const *src, int is_kernel,
                          char **out, int *out_count,
                          size_t *arg_bytes, size_t max_bytes)
{
    task_t *t = proc_current();
    int count = 0;

    while (count < MAX_ARG_STRINGS) {
        char *ptr;
        if (is_kernel) {
            ptr = src[count];
        } else {
            if (copy_from_user(&ptr, &src[count], sizeof(char *)) < 0) {
                /* cleanup */
                for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
                return -EFAULT;
            }
        }
        if (!ptr)
            break;

        out[count] = kmalloc(MAX_ARG_STRLEN);
        if (!out[count]) {
            for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
            return -ENOMEM;
        }

        size_t len;
        if (is_kernel) {
            len = strlen(ptr) + 1;
            if (len > MAX_ARG_STRLEN) {
                kfree(out[count]); out[count] = NULL;
                for (int i = 0; i < count; i++) { kfree(out[i]); out[i] = NULL; }
                return -E2BIG;
            }
            memcpy(out[count], ptr, len);
        } else {
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
        }

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
        if (is_kernel) {
            extra = src[count];
        } else if (copy_from_user(&extra, &src[count], sizeof(char *)) < 0) {
            extra = NULL;
        }
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
    char magic[2];
    int n = vfs_read(fd, magic, sizeof(magic));
    vfs_lseek(fd, 0, SEEK_SET);
    if (n < 2 || magic[0] != '#' || magic[1] != '!')
        return 0;   /* not a script, caller should try ELF */

    /* Read the full first line (up to 256 bytes) */
    char buf[256];
    n = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_lseek(fd, 0, SEEK_SET);
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
                                       char *const *k_envp)
{
    struct a20_ht_internal *ht = a20_ht_create();
    if (ht) t->scratch_buf = ht;

    uint32_t stdin_h = 0xFFFFFFFF, stdout_h = 0xFFFFFFFF, stderr_h = 0xFFFFFFFF;
    if (ht) {
        int console_rd = vfs_open("/dev/console", O_RDONLY, 0);
        if (console_rd >= 0) {
            int64_t h = a20_handle_install(ht, (void *)(uintptr_t)console_rd,
                                           A20_OBJ_FILE, A20_RIGHT_READ | A20_RIGHT_DUP);
            if (h >= 0) stdin_h = (uint32_t)h;
        }
        int console_wr = vfs_open("/dev/console", O_WRONLY, 0);
        if (console_wr >= 0) {
            int64_t h = a20_handle_install(ht, (void *)(uintptr_t)console_wr,
                                           A20_OBJ_FILE, A20_RIGHT_WRITE | A20_RIGHT_DUP);
            if (h >= 0) stdout_h = (uint32_t)h;
        }
        int console_wr2 = vfs_open("/dev/console", O_WRONLY, 0);
        if (console_wr2 >= 0) {
            int64_t h = a20_handle_install(ht, (void *)(uintptr_t)console_wr2,
                                           A20_OBJ_FILE, A20_RIGHT_WRITE | A20_RIGHT_DUP);
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

    return elf_setup_stack_a20(info->stack_top, argc,
                                k_argv, k_envp, info,
                                stdin_h, stdout_h, stderr_h, self_h);
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
    /* ---- 1. Close file descriptors with FD_CLOEXEC ---- */
    fdtable_close_on_exec(t);

    /* ---- 2. Build user stack (Linux or Native ABI) ---- */
    uint64_t sp;
#ifdef CONFIG_ABI_NATIVE
    if (info->is_native_abi) {
        t->abi_mode = 1;
        sp = exec_setup_native_abi(t, info, bprm->argc,
                                    (char *const *)bprm->args,
                                    (char *const *)bprm->envs);
    } else
#endif
    {
        sp = elf_setup_stack(info->stack_top, bprm->argc,
                              (char *const *)bprm->args,
                              (char *const *)bprm->envs, info);
    }
    if (sp == 0)
        return -ENOMEM;

    /* ---- 3. Create new mm BEFORE detaching old ---- */
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
    refcount_set(&new_mm->refcount, 1);
    new_mm->mmap       = info->mmap;

    /* ---- 4. Atomically swap mm ---- */
    mm_struct_t *old_mm    = t->mm;
    uint64_t    *old_pgdir = t->pgdir;

    t->mm   = new_mm;
    t->pgdir = info->pgdir;
    t->entry = info->entry;
    t->ustack = sp;
    strncpy(t->exec_path, abs_path, MAX_PATH_LEN - 1);
    t->exec_path[MAX_PATH_LEN - 1] = '\0';
    const char *base = strrchr(t->exec_path, '/');
    proc_set_name(t, base ? base + 1 : t->exec_path);

    /* ---- 5. Reset signal handlers (POSIX exec semantics) ---- */
    if (t->signals) {
        signal_state_t *ss = (signal_state_t *)t->signals;
        for (int sig = 1; sig < NSIG; sig++) {
            if (ss->actions[sig].sa_handler != SIG_IGN &&
                ss->actions[sig].sa_handler != SIG_DFL)
                ss->actions[sig].sa_handler = SIG_DFL;
            ss->actions[sig].sa_flags  = 0;
            ss->actions[sig].sa_mask   = 0;
        }
        ss->pending = 0;
        memset(ss->pending_has_info, 0, sizeof(ss->pending_has_info));
        memset(ss->pending_info, 0, sizeof(ss->pending_info));
    }
    t->sig_handling       = 0;
    t->thread_pending     = 0;
    t->sigsuspend_active  = 0;

    /* ---- 6. Set up trap context for return to user ---- */
    uint64_t saved_kernel_sp = (uint64_t)(uintptr_t)t->kstack_base + KERNEL_STACK_SIZE;

    if (!t->trap_ctx) {
        uint64_t ks_top = saved_kernel_sp;
        trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
        task_context_t *ctx  = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        memset(ctx, 0, sizeof(*ctx));
        ctx->ra = (uint64_t)user_trap_return;
        ctx->tp = (uint64_t)(uintptr_t)t;
        TASK_CTX_PAGE_TABLE(ctx) = arch_make_satp(info->pgdir);
        t->trap_ctx = trap;
        t->kstack   = (uint64_t)ctx;
    }

    {
        trap_context_t *trap = t->trap_ctx;
        saved_kernel_sp = arch_trap_ctx_get_kernel_stack(trap, saved_kernel_sp);
        memset(trap, 0, sizeof(*trap));
        TRAP_CTX_KScratch0(trap) = arch_make_satp(info->pgdir);
        TRAP_CTX_EPC(trap)       = info->entry;
        TRAP_CTX_SP(trap)        = sp;
        TRAP_CTX_TP(trap)        = info->tls_tp;
#ifdef CONFIG_ABI_NATIVE
        if (info->is_native_abi) {
            TRAP_CTX_SET_ARG0(trap, sp);
        }
#endif
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        arch_trap_ctx_set_kernel_stack(trap, saved_kernel_sp);
        TRAP_CTX_STATUS(trap) = SSTATUS_SPIE | SSTATUS_FS_CLEAN;
    }

    /* ---- 7. Apply SUID/SGID credentials ---- */
    t->exec_load_addr = info->load_addr;
    t->exec_load_size = info->load_size;
    if (st)
        proc_apply_exec_creds(t, st);

    /* ---- 8. Switch page tables, destroy old address space ---- */
    arch_write_satp(arch_make_satp(info->pgdir));
    arch_tlb_flush();

    if (old_mm) {
        mm_destroy(old_mm);
    } else if (old_pgdir && old_pgdir != proc_kernel_pgdir_shared()) {
        pt_destroy_user(old_pgdir);
    }

    arch_fence_i();
    t->state = PROC_RUNNING;
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
    int is_kptr = argv && arch_is_kernel_address(argv);
    int r = exec_copy_args(argv, is_kptr, bprm.args, &bprm.argc,
                           &arg_bytes, MAX_ARG_BYTES);
    if (r < 0) {
        kfree(bprm.path);
        return r;
    }

    int env_is_kptr = envp && arch_is_kernel_address(envp);
    r = exec_copy_args(envp, env_is_kptr, bprm.envs, &bprm.envc,
                       &arg_bytes, MAX_ARG_BYTES);
    if (r < 0) {
        bprm_free(&bprm);
        return r;
    }

    /* ---- 2. Resolve absolute path for exec_path ---- */
    char abs_path[MAX_PATH_LEN];
    if (path[0] == '/') {
        strncpy(abs_path, path, MAX_PATH_LEN - 1);
        abs_path[MAX_PATH_LEN - 1] = '\0';
    } else {
        const char *cwd = t->fs.cwd;
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(abs_path, MAX_PATH_LEN, "%s%s", cwd, path);
        else
            snprintf(abs_path, MAX_PATH_LEN, "%s/%s", cwd, path);
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

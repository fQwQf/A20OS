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

static void proc_apply_exec_creds(task_t *t, const kstat_t *st)
{
    if (!t || !st)
        return;

    int old_uid = t->cred.uid;
    int old_euid = t->cred.euid;
    int old_suid = t->cred.suid;

    if (st->st_mode & S_ISUID) {
        t->cred.euid = (int)st->st_uid;
        t->cred.suid = t->cred.euid;
        t->cred.fsuid = t->cred.euid;
    }
    if (st->st_mode & S_ISGID) {
        t->cred.egid = (int)st->st_gid;
        t->cred.sgid = t->cred.egid;
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

static int exec_file_is_shebang(int fd)
{
    char magic[2];
    int n = vfs_read(fd, magic, sizeof(magic));
    vfs_lseek(fd, 0, SEEK_SET);
    return n >= 2 && magic[0] == '#' && magic[1] == '!';
}

static int exec_read_shebang(int fd, char *interp_path, size_t interp_size,
                             char *interp_arg, size_t arg_size)
{
    char buf[128];
    int n;
    char *cp;
    char *start;
    size_t len;

    if (!interp_path || interp_size == 0)
        return -EINVAL;

    vfs_lseek(fd, 0, SEEK_SET);
    n = vfs_read(fd, buf, sizeof(buf) - 1);
    vfs_lseek(fd, 0, SEEK_SET);
    if (n < 2 || buf[0] != '#' || buf[1] != '!')
        return -ENOEXEC;

    buf[n] = '\0';
    cp = buf + 2;
    while (*cp == ' ' || *cp == '\t')
        ++cp;
    if (*cp == '\0' || *cp == '\n' || *cp == '\r')
        return -ENOEXEC;

    start = cp;
    while (*cp && *cp != '\n' && *cp != '\r' && *cp != ' ' && *cp != '\t')
        ++cp;
    len = (size_t)(cp - start);
    if (len == 0 || len >= interp_size)
        return -ENOEXEC;
    memcpy(interp_path, start, len);
    interp_path[len] = '\0';

    if (interp_arg && arg_size > 0) {
        interp_arg[0] = '\0';
        while (*cp == ' ' || *cp == '\t')
            ++cp;
        if (*cp && *cp != '\n' && *cp != '\r') {
            start = cp;
            while (*cp && *cp != '\n' && *cp != '\r')
                ++cp;
            len = (size_t)(cp - start);
            if (len >= arg_size)
                return -ENOEXEC;
            memcpy(interp_arg, start, len);
            interp_arg[len] = '\0';
        }
    }

    return 0;
}

static void exec_free_args(char *argv[], int argc, char *envp[], int envc,
                           char *path)
{
    for (int i = 0; i < argc; i++)
        kfree(argv[i]);
    for (int i = 0; i < envc; i++)
        kfree(envp[i]);
    kfree(path);
}

static int exec_shebang_interpreter(const char *interp_path, char *argv[],
                                    char *const envp[])
{
    int ret = proc_exec(interp_path, argv, envp);

    if (ret == -ENOENT && strcmp(interp_path, "/bin/busybox") == 0) {
        argv[0] = "./busybox";
        ret = proc_exec("./busybox", argv, envp);
    }

    return ret;
}

int proc_exec(const char *path, char *const argv[], char *const envp[])
{
    task_t *t = proc_current();
    int fd = vfs_open(path, O_RDONLY, 0);
    if (fd < 0)
        return fd;

    kstat_t exec_st;
    int exec_stat_ok = vfs_stat(path, &exec_st);
    if (exec_stat_ok == 0) {
        if ((exec_st.st_mode & S_IFMT) == S_IFDIR) {
            vfs_close(fd);
            return -EACCES;
        }
        if ((exec_st.st_mode & S_IFMT) != S_IFREG) {
            vfs_close(fd);
            return -EACCES;
        }
        int is_script = exec_file_is_shebang(fd);
        if (exec_st.st_mode & 0111) {
            int xr = vfs_faccessat2(AT_FDCWD, path, X_OK, AT_EACCESS);
            if (xr < 0) {
                vfs_close(fd);
                return xr;
            }
        } else if (!is_script) {
            vfs_close(fd);
            return -EACCES;
        }
    }

    char abs_path[MAX_PATH_LEN];
    if (path[0] == '/') {
        strncpy(abs_path, path, MAX_PATH_LEN - 1);
        abs_path[MAX_PATH_LEN - 1] = '\0';
    } else {
        const char *cwd = t ? t->fs.cwd : "/";
        size_t cwd_len = strlen(cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] == '/')
            snprintf(abs_path, MAX_PATH_LEN, "%s%s", cwd, path);
        else
            snprintf(abs_path, MAX_PATH_LEN, "%s/%s", cwd, path);
    }

    char *k_path = kmalloc(strlen(path) + 1);
    if (k_path)
        strcpy(k_path, path);

    char *k_argv[MAX_ARG_STRINGS + 1] = {0};
    char *k_envp[MAX_ARG_STRINGS + 1] = {0};
    int argc = 0, envc = 0;
    size_t arg_bytes = 0;
    int arg_error = 0;

    auto int is_kptr(const void *p) {
        return arch_is_kernel_address(p);
    }

    int argv_is_kernel = argv && is_kptr(argv);
    if (argv) {
        while (argc < MAX_ARG_STRINGS) {
            char *arg;
            if (argv_is_kernel) {
                arg = argv[argc];
            } else {
                if (copy_from_user(&arg, &argv[argc], sizeof(char *)) < 0) {
                    arg_error = -EFAULT;
                    break;
                }
            }
            if (!arg)
                break;
            k_argv[argc] = kmalloc(MAX_ARG_STRLEN);
            if (!k_argv[argc]) {
                arg_error = -ENOMEM;
                break;
            }
            if (argv_is_kernel) {
                size_t len = strlen(arg) + 1;
                if (len > MAX_ARG_STRLEN) {
                    kfree(k_argv[argc]);
                    k_argv[argc] = NULL;
                    arg_error = -E2BIG;
                    break;
                }
                memcpy(k_argv[argc], arg, len);
                arg_bytes += len;
            } else {
                long copied = user_strncpy(k_argv[argc], arg, MAX_ARG_STRLEN);
                if (copied < 0) {
                    kfree(k_argv[argc]);
                    k_argv[argc] = NULL;
                    arg_error = (int)copied;
                    break;
                }
                if ((size_t)copied >= MAX_ARG_STRLEN - 1) {
                    kfree(k_argv[argc]);
                    k_argv[argc] = NULL;
                    arg_error = -E2BIG;
                    break;
                }
                arg_bytes += (size_t)copied + 1;
            }
            if (arg_bytes > MAX_ARG_BYTES || arg_bytes > (t ? t->limits.stack / 4 : MAX_ARG_BYTES)) {
                kfree(k_argv[argc]);
                k_argv[argc] = NULL;
                arg_error = -E2BIG;
                break;
            }
            argc++;
        }
        if (!arg_error && argc == MAX_ARG_STRINGS) {
            char *extra;
            if (argv_is_kernel)
                extra = argv[argc];
            else if (copy_from_user(&extra, &argv[argc], sizeof(char *)) < 0)
                extra = NULL;
            if (extra)
                arg_error = -E2BIG;
        }
    }

    int envp_is_kernel = envp && is_kptr(envp);
    if (!arg_error && envp) {
        while (envc < MAX_ARG_STRINGS) {
            char *env;
            if (envp_is_kernel) {
                env = envp[envc];
            } else {
                if (copy_from_user(&env, &envp[envc], sizeof(char *)) < 0) {
                    arg_error = -EFAULT;
                    break;
                }
            }
            if (!env)
                break;
            k_envp[envc] = kmalloc(MAX_ARG_STRLEN);
            if (!k_envp[envc]) {
                arg_error = -ENOMEM;
                break;
            }
            if (envp_is_kernel) {
                size_t len = strlen(env) + 1;
                if (len > MAX_ARG_STRLEN) {
                    kfree(k_envp[envc]);
                    k_envp[envc] = NULL;
                    arg_error = -E2BIG;
                    break;
                }
                memcpy(k_envp[envc], env, len);
                arg_bytes += len;
            } else {
                long copied = user_strncpy(k_envp[envc], env, MAX_ARG_STRLEN);
                if (copied < 0) {
                    kfree(k_envp[envc]);
                    k_envp[envc] = NULL;
                    arg_error = (int)copied;
                    break;
                }
                if ((size_t)copied >= MAX_ARG_STRLEN - 1) {
                    kfree(k_envp[envc]);
                    k_envp[envc] = NULL;
                    arg_error = -E2BIG;
                    break;
                }
                arg_bytes += (size_t)copied + 1;
            }
            if (arg_bytes > MAX_ARG_BYTES || arg_bytes > (t ? t->limits.stack / 4 : MAX_ARG_BYTES)) {
                kfree(k_envp[envc]);
                k_envp[envc] = NULL;
                arg_error = -E2BIG;
                break;
            }
            envc++;
        }
        if (!arg_error && envc == MAX_ARG_STRINGS) {
            char *extra;
            if (envp_is_kernel)
                extra = envp[envc];
            else if (copy_from_user(&extra, &envp[envc], sizeof(char *)) < 0)
                extra = NULL;
            if (extra)
                arg_error = -E2BIG;
        }
    }

    if (arg_error) {
        vfs_close(fd);
        exec_free_args(k_argv, argc, k_envp, envc, k_path);
        return arg_error;
    }

    if (exec_file_is_shebang(fd)) {
        char interp_path[128];
        char arg_buf[128];
        int sr = exec_read_shebang(fd, interp_path, sizeof(interp_path),
                                   arg_buf, sizeof(arg_buf));
        if (sr == 0) {
            char *new_argv[64];
            int na = 0;
            new_argv[na++] = interp_path;
            if (arg_buf[0])
                new_argv[na++] = arg_buf;
            new_argv[na++] = k_path;
            for (int i = 1; i < argc && na < 63; i++)
                new_argv[na++] = k_argv[i];
            new_argv[na] = NULL;

            vfs_close(fd);
            int ret = exec_shebang_interpreter(interp_path, new_argv, envp);
            exec_free_args(k_argv, argc, k_envp, envc, k_path);
            return ret;
        }
    }

    elf_load_info_t info;
    memset(&info, 0, sizeof(info));
    kdebug("[EXEC] path='%s' k_path='%s' loading ELF\n", path, k_path ? k_path : "(null)");
    int r = elf_load(fd, k_path, &info);
    if (r < 0) {
        kinfo("[EXEC] elf_load failed: r=%d path='%s'\n", (int)r, path);
        if (r == -ENOEXEC) {
            vfs_lseek(fd, 0, SEEK_SET);
            char buf[128];
            int n = vfs_read(fd, buf, sizeof(buf) - 1);
            if (n >= 2 && buf[0] == '#' && buf[1] == '!') {
                buf[n] = '\0';
                char *cp = buf + 2;
                while (*cp == ' ' || *cp == '\t')
                    ++cp;
                if (*cp != '\0' && *cp != '\n') {
                    char *interp_start = cp;
                    while (*cp && *cp != '\n' && *cp != '\r' && *cp != ' ' && *cp != '\t')
                        ++cp;
                    char interp_path[128];
                    size_t ilen = cp - interp_start;
                    if (ilen > 0 && ilen < sizeof(interp_path)) {
                        memcpy(interp_path, interp_start, ilen);
                        interp_path[ilen] = '\0';

                        char arg_buf[128] = {0};
                        int has_arg = 0;
                        while (*cp == ' ' || *cp == '\t')
                            ++cp;
                        if (*cp && *cp != '\n') {
                            char *arg_start = cp;
                            while (*cp && *cp != '\n' && *cp != '\r')
                                ++cp;
                            size_t alen = cp - arg_start;
                            if (alen > 0 && alen < sizeof(arg_buf)) {
                                memcpy(arg_buf, arg_start, alen);
                                arg_buf[alen] = '\0';
                                has_arg = 1;
                            }
                        }

                        char *new_argv[64];
                        int na = 0;
                        new_argv[na++] = interp_path;
                        if (has_arg)
                            new_argv[na++] = arg_buf;
                        new_argv[na++] = k_path;
                        for (int i = 1; i < argc && na < 63; i++)
                            new_argv[na++] = k_argv[i];
                        new_argv[na] = NULL;

                        vfs_close(fd);
                        int ret = proc_exec(interp_path, new_argv, envp);
                        for (int i = 0; i < argc; i++)
                            kfree(k_argv[i]);
                        for (int i = 0; i < envc; i++)
                            kfree(k_envp[i]);
                        kfree(k_path);
                        return ret;
                    }
                }
            }
        }
        /* Not ELF, no shebang — return ENOEXEC like Linux does. */
        vfs_close(fd);
        exec_free_args(k_argv, argc, k_envp, envc, k_path);
        return -ENOEXEC;
    }
    vfs_close(fd);

    fdtable_close_on_exec(t);
    uint64_t sp;

#ifdef CONFIG_ABI_NATIVE
    if (info.is_native_abi) {
        t->abi_mode = 1;
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
        sp = elf_setup_stack_a20(info.stack_top, argc,
                                  (char *const *)k_argv,
                                  (char *const *)k_envp, &info,
                                  stdin_h, stdout_h, stderr_h, self_h);
    } else
#endif
    {
        sp = elf_setup_stack(info.stack_top, argc,
                              (char *const *)k_argv,
                              (char *const *)k_envp, &info);
    }

    mm_struct_t *old_mm = t->mm;
    t->mm = NULL;

    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (!mm) {
        mm_destroy(old_mm);
        exec_free_args(k_argv, argc, k_envp, envc, k_path);
        return -ENOMEM;
    }
    mm->pgdir = info.pgdir;
    mm->brk = info.brk;
    mm->start_brk = info.brk;
    mm->mmap_base = MMAP_BASE_ADDR;
    mm->stack_top = info.stack_top;
    mm->stack_bottom = info.stack_top - USER_STACK_INITIAL_PAGES * PAGE_SIZE;
    mm->total_vm = 0;
    mm->rss = 0;
    refcount_set(&mm->refcount, 1);
    mm->mmap = info.mmap;
    t->mm = mm;

    uint64_t *old_pgdir = t->pgdir;
    t->pgdir = info.pgdir;
    t->entry = info.entry;
    t->ustack = sp;
    strncpy(t->exec_path, abs_path, MAX_PATH_LEN - 1);
    t->exec_path[MAX_PATH_LEN - 1] = '\0';
    const char *base = strrchr(t->exec_path, '/');
    proc_set_name(t, base ? base + 1 : t->exec_path);

    for (int i = 0; i < argc; i++)
        kfree(k_argv[i]);
    for (int i = 0; i < envc; i++)
        kfree(k_envp[i]);

    if (t->signals) {
        signal_state_t *ss = (signal_state_t *)t->signals;
        for (int sig = 1; sig < NSIG; sig++) {
            if (ss->actions[sig].sa_handler != SIG_IGN &&
                ss->actions[sig].sa_handler != SIG_DFL)
                ss->actions[sig].sa_handler = SIG_DFL;
            ss->actions[sig].sa_flags = 0;
            ss->actions[sig].sa_mask = 0;
        }
        ss->pending = 0;
        memset(ss->pending_has_info, 0, sizeof(ss->pending_has_info));
        memset(ss->pending_info, 0, sizeof(ss->pending_info));
    }
    t->sig_handling = 0;
    t->thread_pending = 0;
    t->sigsuspend_active = 0;

    uint64_t saved_entry = info.entry;
    uint64_t saved_sp = sp;
    uint64_t saved_kernel_sp = (uint64_t)(uintptr_t)t->kstack_base + KERNEL_STACK_SIZE;

    if (!t->trap_ctx) {
        uint64_t ks_top = (uint64_t)(uintptr_t)t->kstack_base + KERNEL_STACK_SIZE;
        trap_context_t *trap = (trap_context_t *)(ks_top - sizeof(trap_context_t));
        task_context_t *ctx = (task_context_t *)((uint64_t)trap - sizeof(task_context_t));
        memset(ctx, 0, sizeof(*ctx));
        ctx->ra = (uint64_t)user_trap_return;
        ctx->tp = (uint64_t)(uintptr_t)t;
        TASK_CTX_PAGE_TABLE(ctx) = arch_make_satp(info.pgdir);
        t->trap_ctx = trap;
        t->kstack = (uint64_t)ctx;
        saved_kernel_sp = ks_top;
    }

    {
        trap_context_t *trap = t->trap_ctx;
        saved_kernel_sp = arch_trap_ctx_get_kernel_stack(trap, saved_kernel_sp);
        memset(trap, 0, sizeof(*trap));
        TRAP_CTX_KScratch0(trap) = arch_make_satp(info.pgdir);
        TRAP_CTX_EPC(trap) = saved_entry;
        TRAP_CTX_SP(trap) = saved_sp;
        TRAP_CTX_TP(trap) = info.tls_tp;
        if (info.is_native_abi) {
            TRAP_CTX_SET_ARG0(trap, saved_sp);
        }
        trap->kernel_tp = (uint64_t)(uintptr_t)t;
        arch_trap_ctx_set_kernel_stack(trap, saved_kernel_sp);
        TRAP_CTX_STATUS(trap) = SSTATUS_SPIE | SSTATUS_FS_CLEAN;
    }

    t->exec_load_addr = info.load_addr;
    t->exec_load_size = info.load_size;
    if (exec_stat_ok == 0)
        proc_apply_exec_creds(t, &exec_st);

    arch_write_satp(arch_make_satp(info.pgdir));
    arch_tlb_flush();

    if (old_mm) {
        mm_destroy(old_mm);
    } else if (old_pgdir && old_pgdir != proc_kernel_pgdir_shared()) {
        pt_destroy_user(old_pgdir);
    }

    kfree(k_path);
    arch_fence_i();

    t->state = PROC_RUNNING;
    return 0;
}

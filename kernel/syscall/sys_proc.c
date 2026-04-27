#include "syscall_internal.h"

#define CLD_EXITED     1
#define CLD_KILLED     2
#define CLD_DUMPED     3
#define CLD_STOPPED    5
#define CLD_CONTINUED  6

static uint64_t clamp_stack_rlimit(uint64_t cur, uint64_t max) {
    uint64_t limit = cur < max ? cur : max;
    if (limit > USER_STACK_MAX_SIZE)
        limit = USER_STACK_MAX_SIZE;
    return limit;
}

static uint64_t clamp_nofile_rlimit(uint64_t cur, uint64_t max) {
    uint64_t limit = cur < max ? cur : max;
    if (limit > MAX_FILES)
        limit = MAX_FILES;
    return limit;
}

static void set_uniform_rlimit(uint64_t pair[2], uint64_t limit) {
    pair[0] = limit;
    pair[1] = limit;
}

int64_t sys_exit(int code) {
    proc_exit(code);
    return 0;
}

int64_t sys_exit_group(int code) {
    proc_exit(code);
    return 0;
}

int64_t sys_getpid(void) {
    task_t *t = proc_current();
    return t ? t->pid : 0;
}

int64_t sys_getppid(void) {
    task_t *t = proc_current();
    return t ? t->ppid : 0;
}

int64_t sys_gettid(void) {
    task_t *t = proc_current();
    return t ? t->pid : 0;
}

int64_t sys_set_tid_address(int *tidptr) {
    (void)tidptr;
    return sys_getpid();
}

int64_t sys_set_robust_list(void *head, size_t len) {
    (void)head; (void)len;
    return 0;
}

int64_t sys_getuid(void) { return 0; }
int64_t sys_geteuid(void) { return 0; }
int64_t sys_getgid(void) { return 0; }
int64_t sys_getegid(void) { return 0; }

int64_t sys_getpgid(int pid) {
    (void)pid;
    return sys_getpid();
}

int64_t sys_setpgid(int pid, int pgid) {
    (void)pid; (void)pgid;
    return 0;
}

int64_t sys_setsid(void) {
    return sys_getpid();
}

int64_t sys_clone(uint64_t flags, void *stack, int *ptid, int *ctid, uint64_t tls) {
    return proc_clone(flags, (uint64_t)stack, ptid, ctid, tls);
}

int64_t sys_execve(const char *path, char **argv, char **envp) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long r = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (r < 0) return -EFAULT;
    return proc_exec(kpath, argv, envp);
}

int64_t sys_wait4(int pid, int *status, int options, void *rusage) {
    (void)rusage;
    if (syscall_sig_diag_count < 128) {
        syscall_sig_diag_count++;
        kdebug("[SIGSYS] wait4 pid=%d opt=0x%x\n", pid, options);
    }
    int kstatus = 0;
    int ret = proc_wait4(pid, status ? &kstatus : NULL, options);
    if (ret >= 0 && status) {
        if (copy_to_user(status, &kstatus, sizeof(int)) < 0) return -EFAULT;
    }
    return ret;
}

int64_t sys_waitid(int type, int id, void *info, int options, void *rusage) {
    (void)rusage;
    if (syscall_sig_diag_count < 128) {
        syscall_sig_diag_count++;
        kdebug("[SIGSYS] waitid type=%d id=%d opt=0x%x\n", type, id, options);
    }
    int pid = -1;
    switch (type) {
    case 0: /* P_ALL */
        pid = -1;
        break;
    case 1: /* P_PID */
        pid = id;
        break;
    case 2: /* P_PGID */
        pid = (id == 0) ? 0 : -id;
        break;
    default:
        return -EINVAL;
    }

    int status = 0;
    int ret = proc_wait4(pid, &status, options & 1 /* WNOHANG */);
    if (ret < 0) return ret;

    if (info) {
        uint8_t si[128];
        memset(si, 0, sizeof(si));
        if (ret > 0) {
            int si_code = CLD_EXITED;
            int si_status = 0;
            if ((status & 0xffff) == 0xffff) {
                si_code = CLD_CONTINUED;
            } else if ((status & 0x7f) == 0x7f) {
                si_code = CLD_STOPPED;
                si_status = (status >> 8);
            } else if (status & 0x7f) {
                si_code = (status & 0x80) ? CLD_DUMPED : CLD_KILLED;
                si_status = status & 0x7f;
            } else {
                si_code = CLD_EXITED;
                si_status = (status >> 8) & 0xff;
            }

            ((int *)si)[0] = SIGCHLD;    /* si_signo */
            ((int *)si)[1] = 0;          /* si_errno */
            ((int *)si)[2] = si_code;    /* si_code */
            ((int *)si)[3] = ret;        /* si_pid */
            ((int *)si)[4] = 0;          /* si_uid */
            ((int *)si)[5] = si_status;  /* si_status */
        }
        if (copy_to_user(info, si, sizeof(si)) < 0) return -EFAULT;
    }
    return 0;
}

int64_t sys_sched_yield(void) {
    proc_yield();
    return 0;
}

int64_t sys_reboot(uint64_t magic1, uint64_t magic2, uint64_t cmd) {
    const uint64_t LINUX_REBOOT_MAGIC1 = 0xfee1deadUL;
    const uint64_t LINUX_REBOOT_MAGIC2 = 672274793UL;
    const uint64_t LINUX_REBOOT_MAGIC2A = 85072278UL;
    const uint64_t LINUX_REBOOT_MAGIC2B = 369367448UL;
    const uint64_t LINUX_REBOOT_MAGIC2C = 537993216UL;
    const uint64_t LINUX_REBOOT_CMD_RESTART = 0x01234567UL;
    const uint64_t LINUX_REBOOT_CMD_POWER_OFF = 0x4321fedcUL;
    const uint64_t A20_REBOOT_CMD = 0x424F4F54UL;

    if (magic1 == A20_REBOOT_CMD ||
        (magic1 == LINUX_REBOOT_MAGIC1 &&
         (magic2 == LINUX_REBOOT_MAGIC2 || magic2 == LINUX_REBOOT_MAGIC2A ||
          magic2 == LINUX_REBOOT_MAGIC2B || magic2 == LINUX_REBOOT_MAGIC2C) &&
         cmd == LINUX_REBOOT_CMD_RESTART)) {
        firmware_reboot();
    } else if (magic1 == 0 ||
               (magic1 == LINUX_REBOOT_MAGIC1 &&
                (magic2 == LINUX_REBOOT_MAGIC2 || magic2 == LINUX_REBOOT_MAGIC2A ||
                 magic2 == LINUX_REBOOT_MAGIC2B || magic2 == LINUX_REBOOT_MAGIC2C) &&
                cmd == LINUX_REBOOT_CMD_POWER_OFF)) {
        firmware_shutdown();
    } else {
        return -EINVAL;
    }
    return 0;
}

int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    (void)a1; (void)a2; (void)a3; (void)a4;
    if (op == 15) {
        task_t *t = proc_current();
        if (t) {
            char name[64];
            if (user_strncpy(name, (const char *)a1, sizeof(name)) >= 0)
                proc_set_name(t, name);
        }
    }
    return 0;
}

int64_t sys_prlimit64(int pid, int resource, void *new_rlim, void *old_rlim) {
    (void)pid;
    if (old_rlim) {
        uint64_t r[2] = {0};
        task_t *t = proc_current();
        switch (resource) {
            case 3: set_uniform_rlimit(r, t ? t->rlim_stack : USER_STACK_MAX_SIZE); break;
            case 7: set_uniform_rlimit(r, t ? t->rlim_nofile : MAX_FILES); break;
            default: r[0] = 0; r[1] = (uint64_t)-1; break;
        }
        if (copy_to_user(old_rlim, r, sizeof(r)) < 0) return -EFAULT;
    }
    if (new_rlim) {
        uint64_t r[2];
        if (copy_from_user(r, new_rlim, sizeof(r)) < 0) return -EFAULT;
        task_t *t = proc_current();
        if (!t) return -ESRCH;
        switch (resource) {
            case 3: t->rlim_stack = clamp_stack_rlimit(r[0], r[1]); break;
            case 7: t->rlim_nofile = clamp_nofile_rlimit(r[0], r[1]); break;
            default: break;
        }
    }
    return 0;
}

int64_t sys_getrlimit(int resource, void *rlim) {
    if (!rlim) return -EFAULT;
    uint64_t r[2] = {0};
    task_t *t = proc_current();
    switch (resource) {
        case 3: set_uniform_rlimit(r, t ? t->rlim_stack : USER_STACK_MAX_SIZE); break;
        case 7: set_uniform_rlimit(r, t ? t->rlim_nofile : MAX_FILES); break;
        default: r[0] = 0; r[1] = (uint64_t)-1; break;
    }
    if (copy_to_user(rlim, r, sizeof(r)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_setrlimit(int resource, void *rlim) {
    if (!rlim) return -EFAULT;
    uint64_t r[2];
    if (copy_from_user(r, rlim, sizeof(r)) < 0) return -EFAULT;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    switch (resource) {
        case 3: t->rlim_stack = clamp_stack_rlimit(r[0], r[1]); break;
        case 7: t->rlim_nofile = clamp_nofile_rlimit(r[0], r[1]); break;
        default: break;
    }
    return 0;
}

int64_t sys_getrusage(int who, void *usage) {
    (void)who;
    if (!usage) return -EFAULT;
    uint64_t u[18]; /* 144 bytes / 8 */
    memset(u, 0, sizeof(u));
    task_t *t = proc_current();
    if (t) {
        u[0] = t->total_time / TICKS_PER_SEC;
        u[1] = (t->total_time % TICKS_PER_SEC) * 1000000000UL / TICKS_PER_SEC / 1000;
    }
    if (copy_to_user(usage, u, 144) < 0) return -EFAULT;
    return 0;
}

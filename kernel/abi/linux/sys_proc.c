#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "abi/linux/futex.h"

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
    proc_exit_group(code);
    return 0;
}

int64_t sys_getpid(void) {
    task_t *t = proc_current();
    return t ? (t->tgid ? t->tgid : t->pid) : 0;
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
    task_t *t = proc_current();
    if (t) t->clear_child_tid = tidptr;
    return sys_getpid();
}

int64_t sys_set_robust_list(void *head, size_t len) {
    if (len != sizeof(struct robust_list_head)) return -EINVAL;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    t->robust_list_head = (uintptr_t)head;
    return 0;
}

int64_t sys_get_robust_list(int pid, void *head_ptr, size_t *len_ptr) {
    task_t *t;
    if (pid == 0) {
        t = proc_current();
    } else {
        t = proc_find(pid);
        if (!t) return -ESRCH;
        task_t *cur = proc_current();
        if (cur && t->cred.uid != cur->cred.uid &&
            t->cred.euid != cur->cred.euid &&
            !proc_has_cap(cur, CAP_SETUID))
            return -EPERM;
    }
    if (!t) return -ESRCH;
    uintptr_t head = t->robust_list_head;
    if (copy_to_user(head_ptr, &head, sizeof(head)) < 0) return -EFAULT;
    if (len_ptr) {
        size_t sz = sizeof(struct robust_list_head);
        if (copy_to_user(len_ptr, &sz, sizeof(sz)) < 0) return -EFAULT;
    }
    return 0;
}

static int cred_is_root(task_t *t) {
    return proc_has_cap(t, CAP_SETUID);
}

static int cred_can_setgid(task_t *t) {
    return proc_has_cap(t, CAP_SETGID);
}

static int uid_is_current(task_t *t, int uid) {
    return uid == t->cred.uid || uid == t->cred.euid || uid == t->cred.suid;
}

static int gid_is_current(task_t *t, int gid) {
    return gid == t->cred.gid || gid == t->cred.egid || gid == t->cred.sgid;
}

static void cred_update_uid_caps(task_t *t, int old_uid, int old_euid, int old_suid) {
    if (!t) return;
    int old_had_root = old_uid == 0 || old_euid == 0 || old_suid == 0;
    int new_has_root = t->cred.uid == 0 || t->cred.euid == 0 || t->cred.suid == 0;

    if (old_had_root && !new_has_root) {
        t->cred.cap_effective = 0;
        t->cred.cap_permitted = 0;
        return;
    }
    if (old_euid == 0 && t->cred.euid != 0) {
        t->cred.cap_effective = 0;
        return;
    }
    if (old_euid != 0 && t->cred.euid == 0)
        t->cred.cap_effective = t->cred.cap_permitted;
}

int64_t sys_getuid(void) {
    task_t *t = proc_current();
    return t ? t->cred.uid : 0;
}

int64_t sys_geteuid(void) {
    task_t *t = proc_current();
    return t ? t->cred.euid : 0;
}

int64_t sys_getgid(void) {
    task_t *t = proc_current();
    return t ? t->cred.gid : 0;
}

int64_t sys_getegid(void) {
    task_t *t = proc_current();
    return t ? t->cred.egid : 0;
}

int64_t sys_setuid(int uid) {
    if (uid < 0) return -EINVAL;
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (cred_is_root(t)) {
        int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
        t->cred.uid = t->cred.euid = t->cred.suid = t->cred.fsuid = uid;
        cred_update_uid_caps(t, old_uid, old_euid, old_suid);
        return 0;
    }
    if (!uid_is_current(t, uid)) return -EPERM;
    int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
    t->cred.euid = t->cred.fsuid = uid;
    cred_update_uid_caps(t, old_uid, old_euid, old_suid);
    return 0;
}

int64_t sys_setgid(int gid) {
    if (gid < 0) return -EINVAL;
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (cred_can_setgid(t)) {
        t->cred.gid = t->cred.egid = t->cred.sgid = t->cred.fsgid = gid;
        return 0;
    }
    if (!gid_is_current(t, gid)) return -EPERM;
    t->cred.egid = t->cred.fsgid = gid;
    return 0;
}

int64_t sys_setreuid(int ruid, int euid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (ruid < -1 || euid < -1) return -EINVAL;
    int privileged = cred_is_root(t);
    if (!privileged) {
        if (ruid != -1 && !uid_is_current(t, ruid)) return -EPERM;
        if (euid != -1 && !uid_is_current(t, euid)) return -EPERM;
    }
    int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
    if (ruid != -1) t->cred.uid = ruid;
    if (euid != -1) t->cred.euid = t->cred.fsuid = euid;
    if (ruid != -1 || euid != -1) t->cred.suid = t->cred.euid;
    cred_update_uid_caps(t, old_uid, old_euid, old_suid);
    return 0;
}

int64_t sys_setregid(int rgid, int egid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (rgid < -1 || egid < -1) return -EINVAL;
    if (!cred_can_setgid(t)) {
        if (rgid != -1 && !gid_is_current(t, rgid)) return -EPERM;
        if (egid != -1 && !gid_is_current(t, egid)) return -EPERM;
    }
    if (rgid != -1) t->cred.gid = rgid;
    if (egid != -1) t->cred.egid = t->cred.fsgid = egid;
    if (rgid != -1 || egid != -1) t->cred.sgid = t->cred.egid;
    return 0;
}

int64_t sys_setresuid(int ruid, int euid, int suid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (ruid < -1 || euid < -1 || suid < -1) return -EINVAL;
    int privileged = cred_is_root(t);
    if (!privileged) {
        if (ruid != -1 && !uid_is_current(t, ruid)) return -EPERM;
        if (euid != -1 && !uid_is_current(t, euid)) return -EPERM;
        if (suid != -1 && !uid_is_current(t, suid)) return -EPERM;
    }
    int old_uid = t->cred.uid, old_euid = t->cred.euid, old_suid = t->cred.suid;
    if (ruid != -1) t->cred.uid = ruid;
    if (euid != -1) t->cred.euid = t->cred.fsuid = euid;
    if (suid != -1) t->cred.suid = suid;
    cred_update_uid_caps(t, old_uid, old_euid, old_suid);
    return 0;
}

int64_t sys_getresuid(int *ruid, int *euid, int *suid) {
    task_t *t = proc_current();
    int ids[3] = { t ? t->cred.uid : 0, t ? t->cred.euid : 0, t ? t->cred.suid : 0 };
    if (copy_to_user(ruid, &ids[0], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(euid, &ids[1], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(suid, &ids[2], sizeof(int)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_setresgid(int rgid, int egid, int sgid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (rgid < -1 || egid < -1 || sgid < -1) return -EINVAL;
    if (!cred_can_setgid(t)) {
        if (rgid != -1 && !gid_is_current(t, rgid)) return -EPERM;
        if (egid != -1 && !gid_is_current(t, egid)) return -EPERM;
        if (sgid != -1 && !gid_is_current(t, sgid)) return -EPERM;
    }
    if (rgid != -1) t->cred.gid = rgid;
    if (egid != -1) t->cred.egid = t->cred.fsgid = egid;
    if (sgid != -1) t->cred.sgid = sgid;
    return 0;
}

int64_t sys_getresgid(int *rgid, int *egid, int *sgid) {
    task_t *t = proc_current();
    int ids[3] = { t ? t->cred.gid : 0, t ? t->cred.egid : 0, t ? t->cred.sgid : 0 };
    if (copy_to_user(rgid, &ids[0], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(egid, &ids[1], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(sgid, &ids[2], sizeof(int)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_setfsuid(int uid) {
    task_t *t = proc_current();
    if (!t) return 0;
    int old = t->cred.fsuid;
    if (uid >= 0 && (cred_is_root(t) || uid_is_current(t, uid)))
        t->cred.fsuid = uid;
    return old;
}

int64_t sys_setfsgid(int gid) {
    task_t *t = proc_current();
    if (!t) return 0;
    int old = t->cred.fsgid;
    if (gid >= 0 && (cred_can_setgid(t) || gid_is_current(t, gid)))
        t->cred.fsgid = gid;
    return old;
}

int64_t sys_getpgid(int pid) {
    task_t *self = proc_current();
    task_t *t = pid == 0 ? self : proc_find(pid);
    if (!t) return -ESRCH;
    return t->pgid;
}

int64_t sys_setpgid(int pid, int pgid) {
    task_t *self = proc_current();
    task_t *t = pid == 0 ? self : proc_find(pid);
    if (!self || !t) return -ESRCH;
    if (pgid == 0) pgid = t->pid;
    if (pgid <= 0) return -EINVAL;
    if (t != self && t->ppid != self->pid) return -ESRCH;
    if (t->sid != self->sid) return -EPERM;
    t->pgid = pgid;
    return 0;
}

int64_t sys_setsid(void) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    t->sid = t->pid;
    t->pgid = t->pid;
    return t->sid;
}

int64_t sys_getsid(int pid) {
    task_t *self = proc_current();
    task_t *t = pid == 0 ? self : proc_find(pid);
    if (!t) return -ESRCH;
    return t->sid;
}

static char g_hostname[65] = "A20OS";
static char g_domainname[65] = "";

int64_t sys_sethostname(const char *name, size_t len) {
    if (!name) return -EFAULT;
    if (len >= sizeof(g_hostname)) return -EINVAL;
    task_t *t = proc_current();
    if (!t || (t->cred.uid != 0 && t->cred.euid != 0))
        return -EPERM;
    char buf[65];
    if (user_strncpy(buf, name, sizeof(buf)) < 0) return -EFAULT;
    buf[len] = '\0';
    memcpy(g_hostname, buf, len + 1);
    return 0;
}

int64_t sys_setdomainname(const char *name, size_t len) {
    if (!name) return -EFAULT;
    if (len >= sizeof(g_domainname)) return -EINVAL;
    task_t *t = proc_current();
    if (!t || (t->cred.uid != 0 && t->cred.euid != 0))
        return -EPERM;
    char buf[65];
    if (user_strncpy(buf, name, sizeof(buf)) < 0) return -EFAULT;
    buf[len] = '\0';
    memcpy(g_domainname, buf, len + 1);
    return 0;
}

static unsigned int g_personality;

int64_t sys_personality(unsigned int persona) {
    unsigned int old = g_personality;
    if (persona != 0xffffffffU)
        g_personality = persona;
    return (int64_t)old;
}

int64_t sys_vhangup(void) {
    return 0;
}

int64_t sys_unshare(int flags) {
    if (flags & ~(0x00000100U | 0x00000200U | 0x00000400U | 0x00020000U |
                  0x04000000U | 0x08000000U | 0x10000000U | 0x20000000U |
                  0x40000000U | 0x80000000U))
        return -EINVAL;
    return 0;
}

int64_t sys_pivot_root(const char *new_root, const char *put_old) {
    (void)new_root;
    (void)put_old;
    return -EPERM;
}

int64_t sys_get_mempolicy(int *policy, unsigned long *nmask,
                          unsigned long maxnode, unsigned long addr,
                          unsigned long flags) {
    (void)policy; (void)nmask; (void)maxnode;
    (void)addr; (void)flags;
    return -ENOSYS;
}

struct clone3_args {
    uint64_t flags;
    uint64_t pidfd;
    uint64_t child_tid;
    uint64_t parent_tid;
    uint64_t exit_signal;
    uint64_t stack;
    uint64_t stack_size;
    uint64_t tls;
    uint64_t set_tid;
    uint64_t set_tid_size;
    uint64_t cgroup;
};

int64_t sys_clone3(void *cl_args, size_t size) {
    if (!cl_args) return -EFAULT;
    if (size < 64) return -EINVAL;
    struct clone3_args args;
    memset(&args, 0, sizeof(args));
    size_t cpysz = size < sizeof(args) ? size : sizeof(args);
    if (copy_from_user(&args, cl_args, cpysz) < 0) return -EFAULT;
    return proc_clone(args.flags, args.stack, (int *)args.parent_tid, args.tls, (int *)args.child_tid, (int)args.exit_signal);
}

int64_t sys_openat2(int dirfd, const char *pathname, const void *how, size_t size) {
    (void)how;
    (void)size;
    return sys_openat(dirfd, pathname, 0, 0);
}

int64_t sys_clone(uint64_t flags, void *stack, int *ptid, uint64_t tls, int *ctid) {
    return proc_clone(flags, (uint64_t)stack, ptid, tls, ctid, (int)(flags & 0xFF));
}

int64_t sys_execve(const char *path, char **argv, char **envp) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long r = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (r < 0) return -EFAULT;
    return proc_exec(kpath, argv, envp);
}

int64_t sys_wait4(int pid, int *status, int options, void *rusage) {
    int kstatus = 0;
    int ret = proc_wait4(pid, status ? &kstatus : NULL, options);
    if (ret >= 0 && status) {
        if (copy_to_user(status, &kstatus, sizeof(int)) < 0) return -EFAULT;
    }
    if (ret >= 0 && rusage) {
        char zero_rusage[144];
        memset(zero_rusage, 0, sizeof(zero_rusage));
        if (copy_to_user(rusage, zero_rusage, sizeof(zero_rusage)) < 0) return -EFAULT;
    }
    return ret;
}

#define WNOHANG   1
#define WEXITED   4
#define WSTOPPED  2
#define WCONTINUED 8
#define WNOWAIT   0x1000000

int64_t sys_waitid(int type, int id, void *info, int options, void *rusage) {
    (void)rusage;
    if (!(options & (WEXITED|WSTOPPED|WCONTINUED)))
        options |= WEXITED;

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
    int ret = proc_wait4(pid, &status, options & WNOHANG);
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
            ((int *)si)[4] = proc_current() ? proc_current()->cred.uid : 0; /* si_uid */
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
    const uint64_t LINUX_REBOOT_CMD = 0x424F4F54UL;

    if (magic1 == LINUX_REBOOT_CMD ||
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
    task_t *t = proc_current();
    if (op == PR_SET_NAME) {
        task_t *t = proc_current();
        if (t) {
            char name[64];
            if (user_strncpy(name, (const char *)a1, sizeof(name)) >= 0)
                proc_set_name(t, name);
        }
        return 0;
    }
    if (op == PR_CAPBSET_READ) {
        if (!t || a1 >= 64) return -EINVAL;
        return (t->cred.cap_bounding & (1ULL << a1)) ? 1 : 0;
    }
    if (op == PR_CAPBSET_DROP) {
        if (!t || a1 >= 64) return -EINVAL;
        if (!proc_has_cap(t, CAP_SETPCAP)) return -EPERM;
        t->cred.cap_bounding &= ~(1ULL << a1);
        return 0;
    }
    if (op == PR_SET_THP_DISABLE) {
        if (!t) return -ESRCH;
        if (a1 > 1 || a2 || a3 || a4) return -EINVAL;
        t->policy.thp_disabled = (int)a1;
        return 0;
    }
    if (op == PR_GET_THP_DISABLE)
        return t ? t->policy.thp_disabled : 0;
    return -EINVAL;
}

int64_t sys_prlimit64(int pid, int resource, void *new_rlim, void *old_rlim) {
    (void)pid;
    if (old_rlim) {
        uint64_t r[2] = {0};
        task_t *t = proc_current();
        switch (resource) {
            case 3: set_uniform_rlimit(r, t ? t->limits.stack : USER_STACK_MAX_SIZE); break;
            case 7: set_uniform_rlimit(r, t ? t->limits.nofile : MAX_FILES); break;
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
            case 3: t->limits.stack = clamp_stack_rlimit(r[0], r[1]); break;
            case 7: t->limits.nofile = clamp_nofile_rlimit(r[0], r[1]); break;
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
        case 3: set_uniform_rlimit(r, t ? t->limits.stack : USER_STACK_MAX_SIZE); break;
        case 7: set_uniform_rlimit(r, t ? t->limits.nofile : MAX_FILES); break;
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
        case 3: t->limits.stack = clamp_stack_rlimit(r[0], r[1]); break;
        case 7: t->limits.nofile = clamp_nofile_rlimit(r[0], r[1]); break;
        default: break;
    }
    return 0;
}

int64_t sys_getrusage(int who, void *usage) {
    if (!usage) return -EFAULT;
    uint64_t u[18]; /* 144 bytes / 8 */
    memset(u, 0, sizeof(u));
    task_t *t = proc_current();
    if (t) {
        uint64_t ticks = t->total_time;
        if (who == -1)
            ticks = t->child_utime + t->child_stime;
        u[0] = ticks / 100;
        u[1] = (ticks % 100) * 10000;
    }
    if (copy_to_user(usage, u, 144) < 0) return -EFAULT;
    return 0;
}

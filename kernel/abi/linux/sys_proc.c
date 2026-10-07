#define LINUX_SYSCALL_DECLARE_PROTOTYPES
#include "syscall_impl.h"
#include "abi/linux/futex.h"
#include "abi/linux/fcntl.h"
#include "mm/fault.h"
#include "fs/procfs.h"
#include "fs/vfs/mntns.h"
#include "fs/vfs/mount.h"
#include "fs/vfs/stat_perm.h"
#include "ipc/kexec.h"
#include "ipc/seccomp.h"
#include "sys/usercopy.h"
#include "proc/proc_internal.h"
#include "proc/cred.h"
#include "proc/signal.h"
#include "mm/pt.h"

__attribute__((weak)) int64_t sys_set_thread_area(void *ptr) {
    task_t *t = proc_current();
    if (!t || !t->trap_ctx)
        return -ESRCH;
    /* RISC-V exposes the user TLS pointer through x4 (tp).  musl uses this
     * syscall during startup on configurations where the generic syscall
     * table still contains set_thread_area. */
    TRAP_CTX_TP(t->trap_ctx) = (uint64_t)(uintptr_t)ptr;
    return 0;
}

__attribute__((weak)) int64_t sys_get_thread_area(void *ptr) {
    task_t *t = proc_current();
    if (!t || !t->trap_ctx)
        return -ESRCH;
    if (!ptr)
        return -EFAULT;
    /* x86_64 get_thread_area(2): report the FS-base TLS pointer carried in
     * the trap frame.  Linux returns -EINVAL when no descriptor is set;
     * A20OS always has the base available. */
    uint64_t base = TRAP_CTX_TP(t->trap_ctx);
    if (copy_to_user(ptr, &base, sizeof(uint64_t)) < 0)
        return -EFAULT;
    return 0;
}

__attribute__((weak)) int64_t sys_pause(void) {
    task_t *t = proc_current();
    if (!t)
        return -EINTR;
    /* pause(2): block until a signal arrives.  The park is interruptible;
     * any delivered signal wakes it and pause returns -EINTR (or the signal
     * handler runs first, mirroring Linux). */
    for (;;) {
        if (signal_task_has_unblocked(t))
            return -EINTR;
        if (proc_park_wait(PROC_WAIT_INTERRUPTIBLE, 0) ==
            PROC_WAKE_TIMEOUT_CAPACITY)
            return -EAGAIN;
    }
}
#define CLD_EXITED     1
#define CLD_KILLED     2
#define CLD_DUMPED     3
#define CLD_STOPPED    5
#define CLD_CONTINUED  6

#define LINUX_CLONE_VM       0x00000100ULL
#define LINUX_CLONE_FS       0x00000200ULL
#define LINUX_CLONE_FILES    0x00000400ULL
#define LINUX_CLONE_SIGHAND  0x00000800ULL
#define LINUX_CLONE_PIDFD    0x00001000ULL
#define LINUX_CLONE_PTRACE   0x00002000ULL
#define LINUX_CLONE_VFORK    0x00004000ULL
#define LINUX_CLONE_PARENT   0x00008000ULL
#define LINUX_CLONE_THREAD   0x00010000ULL
#define LINUX_CLONE_SYSVSEM  0x00040000ULL
#define LINUX_CLONE_SETTLS   0x00080000ULL
#define LINUX_CLONE_PARENT_SETTID  0x00100000ULL
#define LINUX_CLONE_CHILD_CLEARTID 0x00200000ULL
#define LINUX_CLONE_CHILD_SETTID   0x01000000ULL
#define LINUX_CLONE_IO         0x80000000ULL
#define LINUX_CLONE_CLEAR_SIGHAND  0x100000000ULL
#define LINUX_CLONE_INTO_CGROUP    0x200000000ULL

#define LINUX_CLONE_SUPPORTED_FLAGS \
    (LINUX_CLONE_VM | LINUX_CLONE_FS | LINUX_CLONE_FILES | \
     LINUX_CLONE_SIGHAND | LINUX_CLONE_PIDFD | LINUX_CLONE_PTRACE | \
     LINUX_CLONE_VFORK | LINUX_CLONE_PARENT | LINUX_CLONE_THREAD | \
     LINUX_CLONE_NEWNS | LINUX_CLONE_NEWPID | LINUX_CLONE_NEWUSER | \
     LINUX_CLONE_SYSVSEM | LINUX_CLONE_SETTLS | \
     LINUX_CLONE_PARENT_SETTID | LINUX_CLONE_CHILD_CLEARTID | \
     LINUX_CLONE_CHILD_SETTID | LINUX_CLONE_IO | 0xFFULL)

/* Namespace types other than mount, pid and user namespaces are not
 * implemented; both clone and clone3 refuse them instead of silently ignoring
 * the flag. */
#define LINUX_CLONE_UNSUPPORTED_NS_FLAGS \
    (LINUX_CLONE_NEWCGROUP | LINUX_CLONE_NEWUTS | LINUX_CLONE_NEWIPC | \
     LINUX_CLONE_NEWNET)

/* CLONE_NEWNS requires privilege in the caller (Linux: CAP_SYS_ADMIN in the
 * current user namespace; simplified here to CAP_SYS_ADMIN or root). */
static int linux_clone_newns_perm_check(void) {
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    if (!proc_has_cap(t, CAP_SYS_ADMIN) && t->cred.euid != 0)
        return -EPERM;
    return 0;
}

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

/*
 * PID_TRANSLATION_CONTRACT:
 * getpid()/gettid()/getppid() report ids in task_active_pid_ns(), i.e. the
 * namespace the caller's CHILDREN join -- task_t::pid_ns_for_children.  Inside
 * a pid namespace that makes the numbers start at 1, which is the whole point:
 * a container's init is pid 1 and its first child is pid 2.
 *
 * getpid() reports the thread-group id and gettid() the task id, so a
 * multithreaded process inside a container shows N threads sharing one
 * container-local group id, as in Linux.  Both go through the same level
 * table on task_t, so t->tgid (a global id) must never be returned directly.
 *
 * The consequence that trips callers up, and is correct: unshare(CLONE_NEWPID)
 * does not change getpid() until the NEXT fork, because the task stays a
 * member of its old namespace and only parks a new one for its children.
 */
int64_t sys_getpid(void) {
    task_t *t = proc_current();
    if (!t) return 0;
    /* The THREAD GROUP's id, not this thread's -- which is why a
     * multithreaded process inside a container has every thread reporting the
     * same getpid() while gettid() differs.  task_t::tgid is a GLOBAL id and
     * would leak the host numbering straight through the namespace. */
    task_t *leader = t->tg_leader ? t->tg_leader : t;
    return task_pid_nr_ns(leader, pidns_current());
}

int64_t sys_getppid(void) {
    task_t *t = proc_current();
    if (!t) return 0;
    /* Linux reports real_parent here so a ptraced child's getppid() does not
     * leak its tracer; the visible id is whatever the parent's namespace
     * makes it, and 0 when the parent is outside the caller's namespace. */
    return task_ppid_nr_ns(t, pidns_current());
}

int64_t sys_gettid(void) {
    task_t *t = proc_current();
    if (!t) return 0;
    return task_pid_nr_ns(t, pidns_current());
}

int64_t sys_set_tid_address(int *tidptr) {
    task_t *t = proc_current();
    if (t) t->clear_child_tid = tidptr;
    return sys_gettid();
}

int64_t sys_set_robust_list(void *head, size_t len) {
    if (len != sizeof(struct robust_list_head)) return -EINVAL;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    t->robust_list_head = (uintptr_t)head;
    return 0;
}

int64_t sys_arch_prctl(int op, uint64_t addr) __attribute__((weak));
int64_t sys_arch_prctl(int op, uint64_t addr) {
    (void)op;
    (void)addr;
    /* arch_prctl is an x86-only syscall; the x86_64 arch port provides the
     * strong definition.  Other platforms have no x86 TLS/segment registers
     * to configure. */
    return -EOPNOTSUPP;
}

int64_t sys_get_robust_list(int pid, void *head_ptr, size_t *len_ptr) {
    task_t *t;
    if (pid == 0) {
        t = proc_get(proc_current());
    } else {
        t = proc_find_get_user(pid);
        if (!t) return -ESRCH;
        task_t *cur = proc_current();
        if (cur && t->cred.uid != cur->cred.uid &&
            t->cred.euid != cur->cred.euid &&
            !proc_has_cap(cur, CAP_SETUID)) {
            proc_put(t);
            return -EPERM;
        }
    }
    if (!t) return -ESRCH;
    uintptr_t head = t->robust_list_head;
    if (copy_to_user(head_ptr, &head, sizeof(head)) < 0) {
        proc_put(t);
        return -EFAULT;
    }
    if (len_ptr) {
        size_t sz = sizeof(struct robust_list_head);
        if (copy_to_user(len_ptr, &sz, sizeof(sz)) < 0) {
            proc_put(t);
            return -EFAULT;
        }
    }
    proc_put(t);
    return 0;
}

/*
 * USERNS_CRED_CONTRACT:
 * task_t::cred stores GLOBAL (host) ids.  Every id that crosses the syscall
 * boundary is translated through the calling task's user namespace, in both
 * directions:
 *   - INBOUND  (setuid/setgid/setreuid/.../setgroups) the id the caller passed
 *     is a namespace-local id and becomes a global one.  An id the namespace
 *     cannot map is EINVAL, never silently accepted: substituting a global id
 *     would let a process name a user its namespace has no authority over.
 *   - OUTBOUND (getuid/geteuid/getgid/getegid/getresuid/getresgid/getgroups)
 *     a global id becomes the namespace-local one, and a global id the
 *     namespace cannot represent is reported as the overflow id (65534)
 *     rather than leaking the host's real numbering.
 *
 * In the initial namespace the maps are the identity, so every one of these
 * is the same integer arithmetic as before and no existing behaviour changes.
 */
static user_namespace_t *sys_cred_ns(void)
{
    return userns_task_own(proc_current());
}

/* Namespace-local id -> global, or -1 when unmapped. */
static int sys_uid_in(int uid)
{
    return userns_to_kuid(sys_cred_ns(), uid);
}

static int sys_gid_in(int gid)
{
    return userns_to_kgid(sys_cred_ns(), gid);
}

int64_t sys_getuid(void) {
    task_t *t = proc_current();
    return t ? userns_from_kuid(sys_cred_ns(), t->cred.uid) : 0;
}

int64_t sys_geteuid(void) {
    task_t *t = proc_current();
    return t ? userns_from_kuid(sys_cred_ns(), t->cred.euid) : 0;
}

int64_t sys_getgid(void) {
    task_t *t = proc_current();
    return t ? userns_from_kgid(sys_cred_ns(), t->cred.gid) : 0;
}

int64_t sys_getegid(void) {
    task_t *t = proc_current();
    return t ? userns_from_kgid(sys_cred_ns(), t->cred.egid) : 0;
}

int64_t sys_setuid(int uid) {
    if (uid < 0) return -EINVAL;
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    int k = sys_uid_in(uid);
    if (k < 0) return -EINVAL;
    return cred_setuid(t, k);
}

int64_t sys_setgid(int gid) {
    if (gid < 0) return -EINVAL;
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    int k = sys_gid_in(gid);
    if (k < 0) return -EINVAL;
    return cred_setgid(t, k);
}

int64_t sys_setreuid(int ruid, int euid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (ruid < -1 || euid < -1) return -EINVAL;
    int kr = -1, ke = -1;
    if (ruid != -1 && (kr = sys_uid_in(ruid)) < 0) return -EINVAL;
    if (euid != -1 && (ke = sys_uid_in(euid)) < 0) return -EINVAL;
    return cred_setreuid(t, kr, ke);
}

int64_t sys_setregid(int rgid, int egid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (rgid < -1 || egid < -1) return -EINVAL;
    int kr = -1, ke = -1;
    if (rgid != -1 && (kr = sys_gid_in(rgid)) < 0) return -EINVAL;
    if (egid != -1 && (ke = sys_gid_in(egid)) < 0) return -EINVAL;
    return cred_setregid(t, kr, ke);
}

int64_t sys_setresuid(int ruid, int euid, int suid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (ruid < -1 || euid < -1 || suid < -1) return -EINVAL;
    int kr = -1, ke = -1, ks = -1;
    if (ruid != -1 && (kr = sys_uid_in(ruid)) < 0) return -EINVAL;
    if (euid != -1 && (ke = sys_uid_in(euid)) < 0) return -EINVAL;
    if (suid != -1 && (ks = sys_uid_in(suid)) < 0) return -EINVAL;
    return cred_setresuid(t, kr, ke, ks);
}

int64_t sys_getresuid(int *ruid, int *euid, int *suid) {
    task_t *t = proc_current();
    user_namespace_t *ns = sys_cred_ns();
    int ids[3] = { t ? userns_from_kuid(ns, t->cred.uid) : 0,
                   t ? userns_from_kuid(ns, t->cred.euid) : 0,
                   t ? userns_from_kuid(ns, t->cred.suid) : 0 };
    if (copy_to_user(ruid, &ids[0], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(euid, &ids[1], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(suid, &ids[2], sizeof(int)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_setresgid(int rgid, int egid, int sgid) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (rgid < -1 || egid < -1 || sgid < -1) return -EINVAL;
    int kr = -1, ke = -1, ks = -1;
    if (rgid != -1 && (kr = sys_gid_in(rgid)) < 0) return -EINVAL;
    if (egid != -1 && (ke = sys_gid_in(egid)) < 0) return -EINVAL;
    if (sgid != -1 && (ks = sys_gid_in(sgid)) < 0) return -EINVAL;
    return cred_setresgid(t, kr, ke, ks);
}

int64_t sys_getresgid(int *rgid, int *egid, int *sgid) {
    task_t *t = proc_current();
    user_namespace_t *ns = sys_cred_ns();
    int ids[3] = { t ? userns_from_kgid(ns, t->cred.gid) : 0,
                   t ? userns_from_kgid(ns, t->cred.egid) : 0,
                   t ? userns_from_kgid(ns, t->cred.sgid) : 0 };
    if (copy_to_user(rgid, &ids[0], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(egid, &ids[1], sizeof(int)) < 0) return -EFAULT;
    if (copy_to_user(sgid, &ids[2], sizeof(int)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_setfsuid(int uid) {
    task_t *t = proc_current();
    if (!t) return 0;
    /* setfsuid reports the OLD fsuid, in the caller's namespace like every
     * other id this call returns. */
    user_namespace_t *ns = sys_cred_ns();
    int old = userns_from_kuid(ns, t->cred.fsuid);
    if (uid < 0)
        return old;
    int k = sys_uid_in(uid);
    if (k < 0)
        return old;   /* Linux: an unmappable id leaves fsuid alone */
    cred_setfsuid(t, k);
    return old;
}

int64_t sys_setfsgid(int gid) {
    task_t *t = proc_current();
    if (!t) return 0;
    user_namespace_t *ns = sys_cred_ns();
    int old = userns_from_kgid(ns, t->cred.fsgid);
    if (gid < 0)
        return old;
    int k = sys_gid_in(gid);
    if (k < 0)
        return old;
    cred_setfsgid(t, k);
    return old;
}

int64_t sys_getpgid(int pid) {
    task_t *self = proc_current();
    task_t *t = pid == 0 ? proc_get(self) : proc_find_get_user(pid);
    if (!t) return -ESRCH;
    int pgid = t->pgid;
    proc_put(t);
    return pgid;
}

int64_t sys_setpgid(int pid, int pgid) {
    task_t *self = proc_current();
    task_t *t = pid == 0 ? proc_get(self) : proc_find_get_user(pid);
    if (!self || !t) {
        proc_put(t);
        return -ESRCH;
    }
    if (pgid == 0) pgid = t->pid;
    if (pgid <= 0) {
        proc_put(t);
        return -EINVAL;
    }
    if (t != self && t->ppid != self->pid) {
        proc_put(t);
        return -ESRCH;
    }
    if (t->sid != self->sid) {
        proc_put(t);
        return -EPERM;
    }
    t->pgid = pgid;
    proc_put(t);
    return 0;
}

int64_t sys_setsid(void) {
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    if (t->pgid == t->pid) return -EPERM;
    t->sid = t->pid;
    t->pgid = t->pid;
    return t->sid;
}

int64_t sys_getsid(int pid) {
    task_t *self = proc_current();
    task_t *t = pid == 0 ? proc_get(self) : proc_find_get_user(pid);
    if (!t) return -ESRCH;
    int sid = t->sid;
    proc_put(t);
    return sid;
}

static char g_hostname[65] = "A20OS";
static char g_domainname[65] = "";

/* Exposed for /proc/sys/kernel/hostname and domainname (procfs render). */
const char *linux_kernel_hostname(void) { return g_hostname; }
const char *linux_kernel_domainname(void) { return g_domainname; }

/* Kernel-side setters used by /proc/sys/kernel writes.  @buf is a kernel
 * buffer; returns 0 or a negative errno. */
int linux_kernel_set_hostname(const char *buf, size_t len)
{
    if (!buf || len >= sizeof(g_hostname))
        return -EINVAL;
    memcpy(g_hostname, buf, len);
    g_hostname[len] = '\0';
    return 0;
}

int linux_kernel_set_domainname(const char *buf, size_t len)
{
    if (!buf || len >= sizeof(g_domainname))
        return -EINVAL;
    memcpy(g_domainname, buf, len);
    g_domainname[len] = '\0';
    return 0;
}

int64_t sys_sethostname(const char *name, size_t len) {
    if (!name) return -EFAULT;
    if (len >= sizeof(g_hostname)) return -EINVAL;
    task_t *t = proc_current();
    if (!t || (t->cred.uid != 0 && t->cred.euid != 0))
        return -EPERM;
    char buf[65];
    if (user_strncpy(buf, name, sizeof(buf)) < 0) return -EFAULT;
    buf[len] = '\0';
    return linux_kernel_set_hostname(buf, len);
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
    return linux_kernel_set_domainname(buf, len);
}

static unsigned int g_personality;

int64_t sys_personality(unsigned int persona) {
    unsigned int old = g_personality;
    if (persona != 0xffffffffU)
        g_personality = persona;
    return (int64_t)old;
}

int64_t sys_vhangup(void) {
    /* vhangup(2) revokes the caller's controlling terminal.  A20OS has no
     * tty layer at all: there is no session leader, no controlling-terminal
     * pointer in task_t, and no tty driver to hang up, so there is no state
     * to revoke.  Reporting success would tell a caller its terminal was
     * revoked when it was not — the fail-closed policy requires reporting
     * "does not exist" instead. */
    return -ENOSYS;
}

int64_t sys_unshare(int flags) {
    /* Real namespace semantics: CLONE_NEWNS, CLONE_NEWPID and CLONE_NEWUSER
     * create real namespaces; every other namespace type is refused with
     * -EINVAL (Linux's error for unsupported types) instead of faking
     * success.  The non-namespace unshare flags (CLONE_FS/FILES/SIGHAND/VM/
     * THREAD/SYSVSEM) are likewise not implemented and refuse honestly. */
    const int known = (int)(LINUX_CLONE_VM | LINUX_CLONE_FS | LINUX_CLONE_FILES |
                      LINUX_CLONE_SIGHAND | LINUX_CLONE_THREAD |
                      LINUX_CLONE_NEWNS | LINUX_CLONE_SYSVSEM |
                      LINUX_CLONE_NEWCGROUP | LINUX_CLONE_NEWUTS |
                      LINUX_CLONE_NEWIPC | LINUX_CLONE_NEWUSER |
                      LINUX_CLONE_NEWPID | LINUX_CLONE_NEWNET);
    if (flags & ~known)
        return -EINVAL;
    if (flags & (int)(LINUX_CLONE_NEWCGROUP | LINUX_CLONE_NEWUTS |
                      LINUX_CLONE_NEWIPC | LINUX_CLONE_NEWNET))
        return -EINVAL;
    if (flags & (int)(LINUX_CLONE_VM | LINUX_CLONE_FS | LINUX_CLONE_FILES |
                      LINUX_CLONE_SIGHAND | LINUX_CLONE_THREAD |
                      LINUX_CLONE_SYSVSEM))
        return -EINVAL;
    /* Linux refuses CLONE_THREAD|CLONE_NEWPID: a thread's whole reason for
     * existing is to share the group, and a namespace whose init thread is
     * not in that group cannot be exited. */
    if ((flags & (int)(LINUX_CLONE_NEWPID | LINUX_CLONE_THREAD)) ==
        (int)(LINUX_CLONE_NEWPID | LINUX_CLONE_THREAD))
        return -EINVAL;

    if (flags & (int)(LINUX_CLONE_NEWNS | LINUX_CLONE_NEWPID)) {
        task_t *t = proc_current();
        if (!t)
            return -ESRCH;
        if (!proc_has_cap(t, CAP_SYS_ADMIN) && t->cred.euid != 0)
            return -EPERM;
    }
    task_t *t = proc_current();
    /* unshare(CLONE_NEWUSER) deliberately needs NO capability: it is how an
     * unprivileged process obtains a namespace it is root in.  The privilege
     * that comes with it is scoped to the new namespace, so it grants nothing
     * outside it.  It runs LAST, because it changes what the other two
     * namespaces are created relative to: a task that unshares NEWNS and
     * NEWUSER in one call wants the mount namespace nested under the user
     * namespace it just joined, not the other way round. */
    mnt_namespace_t *old_mnt = NULL;
    if (flags & (int)LINUX_CLONE_NEWNS) {
        old_mnt = mntns_task_get(t);
        int r = mntns_unshare(t);
        if (r < 0) {
            mntns_put(old_mnt);
            return r;
        }
    }
    if (flags & (int)LINUX_CLONE_NEWPID) {
        int r = pidns_unshare(t);
        if (r < 0) {
            /* Roll the mount namespace back so a multi-flag unshare is not
             * left half applied. */
            if (flags & (int)LINUX_CLONE_NEWNS) {
                mntns_join(t, old_mnt);  /* consumes old_mnt */
            }
            return r;
        }
    }
    if (flags & (int)LINUX_CLONE_NEWUSER) {
        int r = userns_unshare(t);
        if (r < 0)
            return r;
    }
    return 0;
}

int64_t sys_setns(int fd, int nstype) {
    vfile_t *vf = fdtable_get_current_file_ref(fd);
    if (!vf)
        return -EBADF;
    int kind = procfs_ns_file_kind(vf);
    if (kind < 0) {
        /* fd is not a /proc/<pid>/ns/<type> file */
        vfs_put_file(vf);
        return -EINVAL;
    }
    /* Map the target kind to its CLONE_NEW* bit for the nstype check. */
    static const int kind_flags[] = {
        [PROCNS_MNT]    = 0x00020000,  /* CLONE_NEWNS */
        [PROCNS_PID]    = 0x20000000,  /* CLONE_NEWPID */
        [PROCNS_UTS]    = 0x04000000,  /* CLONE_NEWUTS */
        [PROCNS_USER]   = 0x10000000,  /* CLONE_NEWUSER */
        [PROCNS_IPC]    = 0x08000000,  /* CLONE_NEWIPC */
        [PROCNS_NET]    = 0x40000000,  /* CLONE_NEWNET */
        [PROCNS_CGROUP] = 0x02000000,  /* CLONE_NEWCGROUP */
    };
    if (nstype != 0 && nstype != kind_flags[kind]) {
        vfs_put_file(vf);
        return -EINVAL;
    }
    /* Mount, pid and user namespaces can be joined; the other namespace types
     * are system-wide singletons and setns is honestly refused. */
    if (kind != PROCNS_MNT && kind != PROCNS_PID && kind != PROCNS_USER) {
        vfs_put_file(vf);
        return -EINVAL;
    }
    task_t *cur = proc_current();
    int owner_uid = -1;
    int r;
    if (kind == PROCNS_MNT) {
        user_namespace_t *owner = NULL;
        mnt_namespace_t *ns = procfs_ns_file_mntns_get(vf, &owner_uid, &owner);
        if (!ns) {
            vfs_put_file(vf);
            return -EINVAL;
        }
        /* Joining a mount namespace means changing what the caller can see, so
         * the authority required is authority over the USER NAMESPACE THAT
         * OWNS the target -- CAP_SYS_ADMIN in that namespace or in any
         * namespace between the caller's own and it.  A bare proc_has_cap()
         * test would answer "in my own namespace", which is exactly the wrong
         * scope now that a process can be root inside a container and have no
         * authority at all outside it. */
        if (!cur || !owner || !userns_capable(cur, owner, CAP_SYS_ADMIN)) {
            if (owner) userns_put(owner);
            mntns_put(ns);
            vfs_put_file(vf);
            return -EPERM;
        }
        userns_put(owner);
        r = mntns_join(cur, ns);  /* consumes the reference */
        vfs_put_file(vf);
        return r;
    }

    if (kind == PROCNS_PID) {
        user_namespace_t *owner = NULL;
        pid_namespace_t *pns = procfs_ns_file_pidns_get(vf, &owner_uid, &owner);
        if (!pns) {
            vfs_put_file(vf);
            return -EINVAL;
        }
        /* Same scoping rule as the mount case: pid namespaces are resources of
         * a user namespace, and joining one re-points where children join. */
        if (!cur || !owner || !userns_capable(cur, owner, CAP_SYS_ADMIN)) {
            if (owner) userns_put(owner);
            pidns_put(pns);
            vfs_put_file(vf);
            return -EPERM;
        }
        userns_put(owner);
        /* pidns_join() takes its own reference and releases the caller's, so
         * the reference from procfs_ns_file_pidns_get() is dropped here. */
        r = pidns_join(cur, pns);
        pidns_put(pns);
        vfs_put_file(vf);
        return r;
    }

    user_namespace_t *uns = procfs_ns_file_userns_get(vf, &owner_uid);
    if (!uns) {
        vfs_put_file(vf);
        return -EINVAL;
    }
    /* Joining a user namespace is the one setns case with no euid shortcut:
     * the whole point of the namespace is that a process may become a
     * different user inside it, so "already root" would be the wrong test.
     * What is required is authority OVER the target namespace, which is
     * CAP_SYS_ADMIN in the target or in any namespace between the caller's
     * own one and it (userns_capable walks that chain). */
    if (!cur || !userns_capable(cur, uns, CAP_SYS_ADMIN)) {
        userns_put(uns);
        vfs_put_file(vf);
        return -EPERM;
    }
    r = userns_join(cur, uns);
    userns_put(uns);
    vfs_put_file(vf);
    return r;
}

/* Resolve a caller-supplied directory path to the vnode it names, with the
 * vnode reference the caller has to release. */
static vnode_t *pivot_resolve_dir(task_t *cur, const char *user_path,
                                  char *global_out, size_t global_sz,
                                  int *err_out) {
    char kpath[MAX_PATH_LEN];
    long copied = user_strncpy(kpath, user_path, MAX_PATH_LEN);
    if (copied < 0) { *err_out = -EFAULT; return NULL; }
    if (kpath[0] == '\0') { *err_out = -ENOENT; return NULL; }
    if (strlen(global_sz ? global_out : kpath) >= global_sz) {
        *err_out = -ENAMETOOLONG;
        return NULL;
    }
    int pr = syscall_path_at(AT_FDCWD, kpath, global_out, global_sz);
    if (pr < 0) { *err_out = pr; return NULL; }
    vnode_t *vn = vfs_resolve(global_out);
    if (!vn) {
        *err_out = vfs_lookup_errno() ? vfs_lookup_errno() : -ENOENT;
        return NULL;
    }
    if (vn->type != VFS_FT_DIR) {
        vnode_put(vn);
        *err_out = -ENOTDIR;
        return NULL;
    }
    if (vfs_vnode_permission(vn, X_OK) < 0) {
        vnode_put(vn);
        *err_out = -EACCES;
        return NULL;
    }
    (void)cur;
    return vn;
}

/*
 * pivot_root(2), for real.
 *
 * The operation is only meaningful once a process root is a (mount, vnode)
 * pair, which is what this branch introduced: the caller moves its root to
 * another directory and the old root's mount is cut out of the namespace,
 * after which no path resolves into it and only an already-open descriptor
 * can still reach it.
 *
 * Checks, in the order Linux applies them:
 *   1. CAP_SYS_ADMIN;
 *   2. new_root is a directory (and is not the current root);
 *   3. put_old is a directory, lies under the *current* root, and lies under
 *      the *new* root -- that last condition is what stops a process from
 *      using pivot_root to smuggle a reference to a path the new root would
 *      otherwise hide;
 *   4. the new root's mount is not the current root's mount and not below
 *      it, i.e. the pivot may not widen the mount reach;
 * then: detach the old root mount, re-root the process, and move its cwd to
 * put_old's parent directory (Linux semantics -- the process must not be
 * left standing in the old root).
 */
int64_t sys_pivot_root(const char *new_root, const char *put_old) {
    if (!new_root || !put_old) return -EFAULT;
    task_t *cur = proc_current();
    if (!cur) return -ESRCH;
    if (!proc_has_cap(cur, CAP_SYS_ADMIN)) return -EPERM;

    char nr_path[MAX_PATH_LEN];
    char po_path[MAX_PATH_LEN];
    int err = 0;
    vnode_t *nr = pivot_resolve_dir(cur, new_root, nr_path, sizeof(nr_path), &err);
    if (!nr) return err;

    mount_t *old_root_mnt = vfs_task_root_mount(cur);
    vnode_t *old_root_vn = cur->fs.root_vn;

    if (old_root_vn && nr == old_root_vn) {
        vnode_put(nr);
        return -EINVAL;
    }
    if (!nr->mnt) {
        vnode_put(nr);
        return -EINVAL;
    }
    /* Pivoting into the mount you are already rooted in changes nothing, so
     * Linux refuses it.  A *child* of the current root's mount is exactly
     * the normal container pattern (mount a rootfs at /newroot, pivot into
     * it) and stays legal. */
    if (old_root_mnt && nr->mnt == old_root_mnt) {
        vnode_put(nr);
        return -EINVAL;
    }

    vnode_t *po = pivot_resolve_dir(cur, put_old, po_path, sizeof(po_path), &err);
    if (!po) { vnode_put(nr); return err; }

    /* put_old must be under the new root.  Both are already normalized
     * global paths, so this is a boundary-correct prefix test. */
    size_t nr_len = strlen(nr_path);
    while (nr_len > 1 && nr_path[nr_len - 1] == '/')
        nr_path[--nr_len] = '\0';
    if (strcmp(nr_path, po_path) == 0) {
        /* put_old may not be the new root itself. */
        vnode_put(po); vnode_put(nr);
        return -EINVAL;
    }
    if (strncmp(po_path, nr_path, nr_len) != 0 ||
        (po_path[nr_len] != '/' && po_path[nr_len] != '\0')) {
        vnode_put(po); vnode_put(nr);
        return -EINVAL;
    }
    /* ... and it must live in the new root's mount. */
    if (po->mnt != nr->mnt) {
        vnode_put(po); vnode_put(nr);
        return -EINVAL;
    }

    /* put_old's parent becomes the new cwd. */
    char parent[MAX_PATH_LEN];
    strncpy(parent, po_path, sizeof(parent) - 1);
    parent[sizeof(parent) - 1] = '\0';
    size_t plen = strlen(parent);
    while (plen > 1 && parent[plen - 1] == '/')
        parent[--plen] = '\0';
    char *slash = strrchr(parent, '/');
    if (slash == parent)
        parent[1] = '\0';
    else if (slash)
        *slash = '\0';
    vnode_t *cwd_vn = vfs_resolve(parent);
    if (!cwd_vn || cwd_vn->type != VFS_FT_DIR) {
        if (cwd_vn) vnode_put(cwd_vn);
        vnode_put(po); vnode_put(nr);
        return -EINVAL;
    }

    /* Cut the old root out of the namespace first: from here on nothing can
     * reach it by path.  The mount object stays listed in mountinfo, flagged
     * detached, so an open descriptor into it keeps working. */
    if (old_root_mnt && old_root_mnt != nr->mnt)
        vfs_mount_detach_root(old_root_mnt);

    /* Re-root.  The strings follow the objects: root_path is the global
     * spelling of the new root, cwd is its root-relative spelling. */
    vfs_task_root_set(cur, nr->mnt, nr, nr_path);
    const char *root = cur->fs.root_path;
    const char *visible = parent;
    if (strlen(visible) >= strlen(root) &&
        strncmp(visible, root, strlen(root)) == 0)
        visible += strlen(root);
    if (visible[0] == '\0')
        visible = "/";
    vfs_task_cwd_set(cur, cwd_vn, visible);

    vnode_put(cwd_vn);
    vnode_put(po);
    vnode_put(nr);
    return 0;
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
    if (args.flags & ~LINUX_CLONE_SUPPORTED_FLAGS)
        return -EINVAL;
    if (size > sizeof(args)) {
        uint8_t extra[32];
        size_t off = sizeof(args);
        while (off < size) {
            size_t n = size - off;
            if (n > sizeof(extra)) n = sizeof(extra);
            if (copy_from_user(extra, (const char *)cl_args + off, n) < 0)
                return -EFAULT;
            for (size_t i = 0; i < n; i++) {
                if (extra[i] != 0)
                    return -E2BIG;
            }
            off += n;
        }
    }
    if ((args.flags & LINUX_CLONE_SIGHAND) && !(args.flags & LINUX_CLONE_VM))
        return -EINVAL;
    if ((args.flags & LINUX_CLONE_THREAD) && !(args.flags & LINUX_CLONE_SIGHAND))
        return -EINVAL;
    if ((args.flags & LINUX_CLONE_FS) && (args.flags & LINUX_CLONE_NEWNS))
        return -EINVAL;
    if (args.flags & LINUX_CLONE_NEWNS) {
        int perm = linux_clone_newns_perm_check();
        if (perm < 0)
            return perm;
    }
    if (args.flags & LINUX_CLONE_NEWPID) {
        /* Linux: CLONE_THREAD|CLONE_NEWPID is EINVAL -- a thread that does
         * not lead its group cannot host a namespace's pid 1. */
        if (args.flags & LINUX_CLONE_THREAD)
            return -EINVAL;
        int perm = linux_clone_newns_perm_check();
        if (perm < 0)
            return perm;
    }
    if (!!args.stack != !!args.stack_size)
        return -EINVAL;
    if (args.flags & LINUX_CLONE_PIDFD) {
        int tmp;
        if (copy_from_user(&tmp, (const void *)(uintptr_t)args.pidfd, sizeof(tmp)) < 0)
            return -EFAULT;
    }
    uint64_t stack = args.stack;
    if (stack && args.stack_size)
        stack += args.stack_size;
    int pid = proc_clone(args.flags, stack, (int *)(uintptr_t)args.parent_tid, args.tls,
                          (int *)(uintptr_t)args.child_tid, (int)args.exit_signal);
    if (pid < 0 || !(args.flags & LINUX_CLONE_PIDFD))
        return pid;

    int fd = linux_pidfd_create(pid, 0);
    if (fd < 0)
        return fd;
    if (copy_to_user((void *)(uintptr_t)args.pidfd, &fd, sizeof(fd)) < 0) {
        fdtable_close_current(fd);
        return -EFAULT;
    }
    return pid;
}

int64_t sys_openat2(int dirfd, const char *pathname, const void *how, size_t size) {
    struct open_how khow;
    if (!how || !pathname) return -EFAULT;
    if (size < 24 || size > sizeof(khow)) return -EINVAL;
    if (copy_from_user(&khow, how, size < sizeof(khow) ? size : sizeof(khow)) < 0)
        return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long pr0 = user_path_strncpy(kpath, pathname, MAX_PATH_LEN);
    if (pr0 < 0)
        return pr0;
    if (size > sizeof(khow)) {
        uint8_t extra[32];
        size_t off = sizeof(khow);
        while (off < size) {
            size_t n = size - off;
            if (n > sizeof(extra)) n = sizeof(extra);
            if (copy_from_user(extra, (const char *)how + off, n) < 0)
                return -EFAULT;
            for (size_t i = 0; i < n; i++) {
                if (extra[i] != 0)
                    return -E2BIG;
            }
            off += n;
        }
    }
    if (khow.resolve & ~(RESOLVE_NO_SYMLINKS | RESOLVE_BENEATH | RESOLVE_IN_ROOT |
                         RESOLVE_NO_MAGICLINKS | RESOLVE_NO_XDEV |
                         RESOLVE_CACHED))
        return -EINVAL;

    int flags = (int)khow.flags;
    int mode = (int)(khow.mode & 07777);
    /* vfs_openat2() already installed the fd with O_CLOEXEC honoured. */
    return vfs_openat2(dirfd, kpath, flags, mode, khow.resolve);
}

int64_t sys_clone(uint64_t flags, void *stack, int *ptid, uint64_t tls, int *ctid) {
#ifdef CONFIG_NOMMU
    /*
     * Architectures without a dedicated vfork syscall implement libc vfork()
     * as clone(SIGCHLD, 0).  Old Linux ABIs such as ARM's fork syscall are
     * normalized to the same argument tuple by the architecture syscall hook.
     * In a shared physical address space this request must have vfork
     * semantics: share the mm and suspend the parent until exec/exit.
     */
    if (flags == SIGCHLD)
        flags |= LINUX_CLONE_VM | LINUX_CLONE_VFORK;
    if (!(flags & LINUX_CLONE_VM)) {
        return -EINVAL; /* NOMMU does not support fork without CLONE_VM */
    }
#endif
    if (flags & LINUX_CLONE_UNSUPPORTED_NS_FLAGS)
        return -EINVAL;
    if ((flags & LINUX_CLONE_FS) && (flags & LINUX_CLONE_NEWNS))
        return -EINVAL;
    if (flags & LINUX_CLONE_NEWNS) {
        int perm = linux_clone_newns_perm_check();
        if (perm < 0)
            return perm;
    }
    if (flags & (int)LINUX_CLONE_NEWPID) {
        /* Linux: CLONE_THREAD|CLONE_NEWPID is EINVAL -- a thread that does not
         * lead its group cannot host a namespace's pid 1.  Checked here as
         * well as in clone3 so both entry points agree. */
        if (flags & LINUX_CLONE_THREAD)
            return -EINVAL;
        int perm = linux_clone_newns_perm_check();
        if (perm < 0)
            return perm;
    }
    return proc_clone(flags, (uint64_t)(uintptr_t)stack, ptid, tls, ctid,
                      (int)(flags & 0xFF));
}

int64_t sys_execve(const char *path, char **argv, char **envp) {
    if (!path) return -EFAULT;
    char kpath[MAX_PATH_LEN];
    long r = user_strncpy(kpath, path, MAX_PATH_LEN);
    if (r < 0) return -EFAULT;
    /* Path filled the buffer without a NUL: too long */
    if (r >= (long)(MAX_PATH_LEN - 1) && kpath[MAX_PATH_LEN - 2] != '\0')
        return -ENAMETOOLONG;
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
    case 3: /* P_PIDFD */
        pid = linux_pidfd_pid(id);
        if (pid < 0)
            return pid;
        break;
    default:
        return -EINVAL;
    }

    int status = 0;
    int wait_opts = (options & WNOHANG);
    if (options & WSTOPPED) wait_opts |= 2;
    if (options & WCONTINUED) wait_opts |= WCONTINUED;
    if (options & WNOWAIT) wait_opts |= WNOWAIT;
    int ret = proc_wait4(pid, &status, wait_opts);
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
    const uint64_t LINUX_REBOOT_CMD_KEXEC = 0x45584543UL;
    const uint64_t LINUX_REBOOT_CMD = 0x424F4F54UL;

    /* The magic values are public ABI constants, not a secret, so they
     * authorise nothing.  Gate the whole call on CAP_SYS_BOOT, as the kexec
     * entry points in sys_missing.c do. */
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    if (!proc_has_cap(t, CAP_SYS_BOOT) && t->cred.euid != 0)
        return -EPERM;

    if (magic1 == LINUX_REBOOT_CMD ||
        (magic1 == LINUX_REBOOT_MAGIC1 &&
         (magic2 == LINUX_REBOOT_MAGIC2 || magic2 == LINUX_REBOOT_MAGIC2A ||
          magic2 == LINUX_REBOOT_MAGIC2B || magic2 == LINUX_REBOOT_MAGIC2C) &&
         cmd == LINUX_REBOOT_CMD_KEXEC)) {
        /* A staged image exists but no architecture provides the
         * machine_kexec MMU-teardown trampoline yet; Linux reports the
         * same way on boards without a kexec backend. */
        return kexec_is_loaded() ? -ENOSYS : -EINVAL;
    } else if (magic1 == LINUX_REBOOT_CMD ||
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
        /* TEMP-DIAG: audit buddy free-list integrity before poweroff, for the
         * shmring poweroff corruption investigation */
        kinfo("[SD] POWER_OFF entry");
        int audit = pfa_audit_lists();
        kinfo("[SD] audit errors=%d", audit);
#if !defined(CONFIG_NOMMU) && defined(ARCH_HAS_PGTABLE_OPS)
        /* MM_AS_MODEL: prove the per-PTE status and the hardware page tables
         * agreed in every live address space across the whole run. */
        {
            mm_pt_audit_report_t rep;
            mm_pt_audit_all(&rep);
            kinfo("[MM-ASM] pt_pages=%lu entries=%lu missing_meta=%lu "
                  "present=%lu absent=%lu prot=%lu cow=%lu vma=%lu "
                  "vmai=%lu cls=%lu safe=%lu anon_virt=%lu huge=%lu "
                  "huge_install=%lu cow_from_status=%lu "
                  "seg_slots=%lu seg_bad=%lu seg_kind=%lu "
                  "seg_ok=%lu seg_diff=%lu seg_miss=%lu "
                  "seg_dispatch=%lu seg_fallback=%lu\n",
                  (unsigned long)rep.pt_pages, (unsigned long)rep.entries,
                  (unsigned long)rep.missing_meta,
                  (unsigned long)rep.present_mismatch,
                  (unsigned long)rep.absent_mismatch,
                  (unsigned long)rep.prot_mismatch,
                  (unsigned long)rep.cow_mismatch,
                  (unsigned long)rep.vma_mismatch,
                  (unsigned long)rep.vmai_mismatch,
                  (unsigned long)rep.cls_mismatch,
                  (unsigned long)rep.safe_mismatch,
                  (unsigned long)rep.anon_virt,
                  (unsigned long)rep.huge_leaves,
                  (unsigned long)mm_huge_install_count,
                  (unsigned long)mm_cow_from_status_count,
                  (unsigned long)rep.seg_slots,
                  (unsigned long)rep.seg_bad_slot,
                  (unsigned long)rep.seg_kind_mismatch,
                  (unsigned long)mm_seg_shadow_agree,
                  (unsigned long)mm_seg_shadow_disagree,
                  (unsigned long)mm_seg_shadow_miss,
                  (unsigned long)mm_seg_dispatch_seg,
                  (unsigned long)mm_seg_dispatch_fallback);
            kinfo("[MM-ASM]   map list: entries=%lu overlap=%lu dead=%lu "
                  "ok=%lu\n",
                  (unsigned long)rep.seg_extent_vmas,
                  (unsigned long)rep.seg_extent_mismatch,
                  (unsigned long)rep.seg_extent_noseg,
                  (unsigned long)rep.seg_pte_agree);
            kinfo("[MM-ASM]   miss why: hole=%lu leaf=%lu unnamed=%lu "
                  "extent=%lu ambig=%lu bottom=%lu\n",
                  (unsigned long)mm_seg_miss_why[0],
                  (unsigned long)mm_seg_miss_why[1],
                  (unsigned long)mm_seg_miss_why[2],
                  (unsigned long)mm_seg_miss_why[3],
                  (unsigned long)mm_seg_miss_why[4],
                  (unsigned long)mm_seg_miss_why[5]);
            kinfo("[MM-ASM]   annot lost: table_full=%lu nibbles_full=%lu\n",
                  (unsigned long)mm_seg_annot_lost[0],
                  (unsigned long)mm_seg_annot_lost[1]);
            /* The index-overflow fallback.  Zero here is NOT a pass: it means
             * no address space in this run had more than MM_SEG_INDEX_CAPACITY
             * mappings, so the list-walk branch of mm_seg_find() never ran.
             * Read it as "unexercised", not "verified". */
            kinfo("[MM-ASM]   seg index overflow: %lu\n",
                  (unsigned long)mm_seg_index_overflow);
            /* One line per level rather than three inline slots: the array has
             * eight entries and the inline form silently hid levels 3..7,
             * which is precisely where a loss is fine-grained rather than a
             * statement about the root table's four gigabytes. */
            for (int _l = 0; _l < 8; _l++)
                if (mm_seg_full_lvl[_l])
                    kinfo("[MM-ASM]     full at level %d: %lu\n", _l,
                          (unsigned long)mm_seg_full_lvl[_l]);
            if (rep.vma_mismatch)
                kinfo("[MM-ASM]   first vma_mismatch  va=0x%lx\n",
                      (unsigned long)rep.vma_bad_va);
            if (rep.vmai_mismatch)
                kinfo("[MM-ASM]   first vmai_mismatch va=0x%lx\n",
                      (unsigned long)rep.vmai_bad_va);
            if (rep.cls_mismatch)
                kinfo("[MM-ASM]   first cls_mismatch  va=0x%lx cls=%lu\n",
                      (unsigned long)rep.cls_bad_va,
                      (unsigned long)rep.cls_bad_class);
            if (rep.seg_bad_slot)
                kinfo("[MM-ASM]   first seg_bad_slot  va=0x%lx\n",
                      (unsigned long)rep.seg_bad_va);
            if (rep.seg_kind_mismatch)
                kinfo("[MM-ASM]   first seg_kind_mismatch va=0x%lx kind=%lu\n",
                      (unsigned long)rep.seg_kind_bad_va,
                      (unsigned long)rep.seg_kind_bad);
        }
#endif
        firmware_shutdown();
    } else {
        return -EINVAL;
    }
    return 0;
}

int64_t sys_prctl(int op, uint64_t a1, uint64_t a2, uint64_t a3, uint64_t a4) {
    task_t *t = proc_current();
    if (op == PR_SET_PDEATHSIG) {
        /* One-shot signal delivered to the child when its parent dies.  A
         * signal number >= _NSIG is rejected; a parent that is already gone
         * is reported with -ESRCH (Linux semantics). */
        if (!t)
            return -ESRCH;
        if (a1 >= 64)
            return -EINVAL;
        /* The parent's state is sampled in its own critical section before
         * the target's lock is taken, so the two park_locks are never nested
         * (INV-P3).  ->pdeathsig is park_lock-owned and paired with the claim
         * proc_reparent_children() makes under the same lock. */
        task_t *parent = t->parent;
        int parent_dead = 1;
        if (parent) {
            uint64_t ppf = spin_lock_irqsave(&parent->park_lock);
            int pstate = parent->state;
            spin_unlock_irqrestore(&parent->park_lock, ppf);
            parent_dead = (pstate == PROC_ZOMBIE || pstate == PROC_UNUSED);
        }
        if (parent_dead)
            return -ESRCH;
        uint64_t pf = spin_lock_irqsave(&t->park_lock);
        t->pdeathsig = (int)a1;
        spin_unlock_irqrestore(&t->park_lock, pf);
        return 0;
    }
    if (op == PR_GET_PDEATHSIG) {
        if (!t)
            return -ESRCH;
        int sig = t->pdeathsig;
        if (copy_to_user((void *)(uintptr_t)a1, &sig, sizeof(sig)) < 0)
            return -EFAULT;
        return 0;
    }
    if (op == PR_SET_NAME) {
        task_t *t = proc_current();
        if (t) {
            char name[64];
            if (user_strncpy(name, (const char *)(uintptr_t)a1, sizeof(name)) >= 0)
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
    if (op == PR_SET_SECCOMP) {
        /* a1 = mode, a2 = optional sock_fprog pointer for FILTER mode. */
        if (!t)
            return -ESRCH;
        if (a1 == SECCOMP_MODE_STRICT)
            return seccomp_set_strict(t);
        if (a1 == SECCOMP_MODE_FILTER)
            return seccomp_install_filter(t, (const void *)(uintptr_t)a2);
        return -EINVAL;
    }
    if (op == PR_GET_SECCOMP) {
        long mode = seccomp_get_mode(t);
        return mode;
    }
    return -EINVAL;
}

/*
 * Report a task's limit.  An unenforced resource reports RLIM_INFINITY, which
 * is the truthful answer: no limit applies.  It is not a claim that a limit
 * exists and happens to be large.
 */
static int rlimit_get(task_t *t, int resource, uint64_t pair[2])
{
    uint64_t v = 0;
    switch (resource) {
    case RLIMIT_STACK: v = t ? t->limits.stack : USER_STACK_MAX_SIZE; break;
    case RLIMIT_CORE:  v = signal_task_rlim_core(t); break;
    case RLIMIT_NOFILE: v = t ? t->limits.nofile : MAX_FILES; break;
    case RLIMIT_AS:    v = t ? t->limits.as : 0; break;
    case RLIMIT_NPROC: v = t ? t->limits.nproc : 0; break;
    default: return -ENOSYS;
    }
    set_uniform_rlimit(pair, v);
    return 0;
}

/*
 * Install a limit.  A resource with no enforcement returns -EINVAL rather than
 * a silent success: a caller that sets RLIMIT_AS and is told "ok" will believe
 * the address space is capped when nothing caps it.
 */
static int rlimit_set(task_t *t, int resource, uint64_t cur, uint64_t max)
{
    if (!t)
        return -ESRCH;
    switch (resource) {
    case RLIMIT_STACK: t->limits.stack = clamp_stack_rlimit(cur, max); break;
    case RLIMIT_CORE:  signal_task_set_rlim_core(t, cur); break;
    case RLIMIT_NOFILE: t->limits.nofile = clamp_nofile_rlimit(cur, max); break;
    case RLIMIT_AS:    t->limits.as = cur; break;
    case RLIMIT_NPROC: t->limits.nproc = cur; break;
    default: return -EINVAL;
    }
    return 0;
}

int64_t sys_prlimit64(int pid, int resource, void *new_rlim, void *old_rlim) {
    if (resource < 0 || resource >= RLIM_NLIMITS)
        return -EINVAL;
    if (!new_rlim && !old_rlim)
        return 0;

    /* Previously the pid argument was discarded outright, so a supervisor
     * could only ever inspect or change its own limits. */
    task_t *self = proc_current();
    if (!self)
        return -ESRCH;
    task_t *t = self;
    if (pid != 0) {
        t = proc_find_get_user(pid);
        if (!t)
            return -ESRCH;
    }

    int ret = 0;
    if (old_rlim) {
        uint64_t r[2] = { 0, 0 };
        int gr = rlimit_get(t, resource, r);
        if (gr < 0) {
            /* Nothing enforces this resource, so there is no value to report.
             * RLIM_INFINITY is the truth; ENOSYS would be a lie about which
             * resources exist. */
            r[0] = 0;
            r[1] = (uint64_t)-1;
        }
        if (copy_to_user(old_rlim, r, sizeof(r)) < 0)
            ret = -EFAULT;
    }
    if (ret == 0 && new_rlim) {
        /* Changing another task's limits is a privilege operation. */
        if (t != self && !proc_has_cap(self, CAP_SYS_RESOURCE) &&
            t->cred.uid != self->cred.uid) {
            ret = -EPERM;
        } else {
            uint64_t r[2];
            if (copy_from_user(r, new_rlim, sizeof(r)) < 0)
                ret = -EFAULT;
            else
                ret = rlimit_set(t, resource, r[0], r[1]);
        }
    }
    if (t != self)
        proc_put(t);
    return ret;
}

int64_t sys_getrlimit(int resource, void *rlim) {
    if (resource < 0 || resource >= RLIM_NLIMITS)
        return -EINVAL;
    if (!rlim) return -EFAULT;
    task_t *t = proc_current();
    uint64_t r[2] = { 0, 0 };
    if (rlimit_get(t, resource, r) < 0) {
        r[0] = 0;
        r[1] = (uint64_t)-1;
    }
    if (copy_to_user(rlim, r, sizeof(r)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_setrlimit(int resource, void *rlim) {
    if (resource < 0 || resource >= RLIM_NLIMITS)
        return -EINVAL;
    if (!rlim) return -EFAULT;
    uint64_t r[2];
    if (copy_from_user(r, rlim, sizeof(r)) < 0) return -EFAULT;
    task_t *t = proc_current();
    if (!t) return -ESRCH;
    return rlimit_set(t, resource, r[0], r[1]);
}

int64_t sys_getrusage(int who, void *usage) {
    if (who != RUSAGE_SELF && who != RUSAGE_CHILDREN &&
        who != RUSAGE_THREAD)
        return -EINVAL;
    if (!usage) return -EFAULT;
    /* 64-bit struct rusage: two timevals then sixteen longs, matching the
     * u[] slots used below.  Unfilled slots stay zero, which for
     * ixrss/idrss/isrss/nswap/msgsnd/msgrcv is the truthful answer (no
     * such accounting exists) rather than a missing measurement. */
    uint64_t u[18];
    memset(u, 0, sizeof(u));
    task_t *t = proc_current();
    if (t) {
        uint64_t uticks,sticks;
        if (who == RUSAGE_CHILDREN) {
            uticks = t->child_utime;
            sticks = t->child_stime;
        } else {
            uticks = t->utime_ticks;
            sticks = t->stime_ticks;
        }
        /* Accounting runs once per 100 Hz scheduler pass, so ticks/100 is
         * seconds and the remainder is centiseconds -> microseconds. */
        u[0] = uticks / 100;
        u[1] = (uticks % 100) * 10000;
        u[2] = sticks / 100;
        u[3] = (sticks % 100) * 10000;
        u[8] = __atomic_load_n(&t->perf_page_faults, __ATOMIC_RELAXED);
        u[9] = __atomic_load_n(&t->perf_page_faults_maj, __ATOMIC_RELAXED);
        u[11] = __atomic_load_n(&t->io_read_bytes, __ATOMIC_RELAXED);
        u[12] = __atomic_load_n(&t->io_write_bytes, __ATOMIC_RELAXED);
        u[16] = __atomic_load_n(&t->perf_switches_vol, __ATOMIC_RELAXED);
        u[17] = __atomic_load_n(&t->perf_switches_invol, __ATOMIC_RELAXED);
    }
    if (copy_to_user(usage, u, 144) < 0) return -EFAULT;
    return 0;
}

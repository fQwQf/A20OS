#include "syscall_impl.h"
#include "core/version.h"
#include "mm/frame.h"

int64_t sys_uname(void *buf) {
    struct uname { char s[65],n[65],r[65],v[65],m[65],d[65]; };
    if (!buf) return -EFAULT;
    struct uname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.s, "A20OS");
    strcpy(u.n, "AAAAAAAAAAAAAAAAAAAA");
    strcpy(u.r, VERSION);
    strcpy(u.v, "A20OS version " VERSION " (Linux 6.8.0 compatible)");
    strcpy(u.m, ARCH_NAME);
    if (copy_to_user(buf, &u, sizeof(u)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_sysinfo(void *info) {
    if (!info) return -EFAULT;
    /* Match musl struct sysinfo layout on 64-bit (total 112 bytes) */
    struct sysinfo_layout {
        uint64_t uptime;
        uint64_t loads[3];
        uint64_t totalram;
        uint64_t freeram;
        uint64_t sharedram;
        uint64_t bufferram;
        uint64_t totalswap;
        uint64_t freeswap;
        uint16_t procs;
        uint16_t pad[3];
        uint64_t totalhigh;
        uint64_t freehigh;
        uint32_t mem_unit;
        uint32_t _pad;
    } si;
    memset(&si, 0, sizeof(si));

    extern size_t frame_free_count(void);
    si.uptime = timer_get_ticks() / TICKS_PER_SEC;
    si.totalram = pfa.total_frames * PAGE_SIZE;
    si.freeram = frame_free_count() * PAGE_SIZE;
    si.bufferram = 0;
    si.sharedram = 0;
    si.totalswap = 0;
    si.freeswap = 0;
    si.procs = 1;
    si.mem_unit = 1;

    if (copy_to_user(info, &si, sizeof(si)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_getgroups(int size, int *list) {
    task_t *t = proc_current();
    int n = t ? t->cred.ngroups : 0;
    if (size < 0) return -EINVAL;
    if (size == 0) return n;
    if (!list) return -EFAULT;
    if (size < n) return -EINVAL;
    if (n > 0 && copy_to_user(list, t->cred.groups, (size_t)n * sizeof(int)) < 0)
        return -EFAULT;
    return n;
}

int64_t sys_setgroups(size_t size, const int *list) {
    task_t *t = proc_current();
    if (!t) return -EINVAL;
    if (!proc_has_cap(t, CAP_SETGID)) return -EPERM;
    if (size > MAX_GROUPS) return -EINVAL;
    if (size && !list) return -EFAULT;
    int tmp[MAX_GROUPS];
    if (size && copy_from_user(tmp, list, size * sizeof(int)) < 0)
        return -EFAULT;
    for (size_t i = 0; i < size; i++) {
        if (tmp[i] < 0) return -EINVAL;
    }
    for (size_t i = 0; i < size; i++)
        t->cred.groups[i] = tmp[i];
    t->cred.ngroups = (int)size;
    return 0;
}

int64_t sys_umask(int newmask) {
    task_t *t = proc_current();
    if (!t) return 022;
    int old = t->fs.umask;
    t->fs.umask = newmask & 0777;
    return old;
}

int64_t sys_syslog(int type, char *buf, int len) {
    (void)type; (void)buf; (void)len;
    return 0;
}

/* ============================================================
 * Random / Misc
 * ============================================================ */

int64_t sys_getrandom(void *buf, size_t len, int flags) {
    (void)flags;
    if (!buf) return -EFAULT;
    uint8_t tmp[128];
    size_t done = 0;
    while (done < len) {
        size_t chunk = len - done > sizeof(tmp) ? sizeof(tmp) : len - done;
        random_fill(tmp, chunk);
        if (copy_to_user((char*)buf + done, tmp, chunk) < 0) return -EFAULT;
        done += chunk;
    }
    return (int64_t)len;
}

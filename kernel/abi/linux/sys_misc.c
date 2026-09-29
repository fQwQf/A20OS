#include "syscall_impl.h"
#include "core/version.h"
#include "mm/frame.h"

#define LINUX_GRND_NONBLOCK 0x0001U
#define LINUX_GRND_RANDOM   0x0002U
#define LINUX_GRND_INSECURE 0x0004U
#define LINUX_GRND_SUPPORTED \
    (LINUX_GRND_NONBLOCK | LINUX_GRND_RANDOM | LINUX_GRND_INSECURE)

int64_t sys_uname(void *buf) {
    struct uname { char s[65],n[65],r[65],v[65],m[65],d[65]; };
    if (!buf) return -EFAULT;
    struct uname u;
    memset(&u, 0, sizeof(u));
    snprintf(u.s, sizeof(u.s), "%s", A20OS_SYSNAME);
    snprintf(u.n, sizeof(u.n), "%s", A20OS_NODENAME);
    snprintf(u.r, sizeof(u.r), "%s", A20OS_RELEASE);
    snprintf(u.v, sizeof(u.v), "%s", A20OS_VERSION_FULL);
    snprintf(u.m, sizeof(u.m), "%s", ARCH_NAME);
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
#ifdef CONFIG_SWAP
    si.totalswap = total_swap_pages * PAGE_SIZE;
    si.freeswap = nr_swap_pages * PAGE_SIZE;
#else
    si.totalswap = 0;
    si.freeswap = 0;
#endif
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

/* syslog(2) actions, uapi/linux/klog.h.  Only the values A20OS can honour
 * against its klog ring are listed; SYSLOG_ACTION_OPEN/CLOSE (the historic
 * console switch) and SYSLOG_ACTION_SIZE_UNCLEARED have no counterpart. */
#define LINUX_SYSLOG_ACTION_SIZE_BUFFER   2
#define LINUX_SYSLOG_ACTION_CLEAR         3
#define LINUX_SYSLOG_ACTION_READ_ALL      4
#define LINUX_SYSLOG_ACTION_READ_CLEAR    5
#define LINUX_SYSLOG_ACTION_CLEAR_BOOT    6
#define LINUX_SYSLOG_ACTION_CONSOLE_LEVEL 8
#define LINUX_SYSLOG_ACTION_CONSOLE_OFF   9

/*
 * syslog(2) over the kernel klog ring (kernel/core/klog.c).  The ring is the
 * same buffer /dev/kmsg serves, so SYSLOG_ACTION_READ_ALL returns the buffered
 * messages from offset 0 each call and returns 0 once drained — Linux's
 * contract for a non-blocking ring read.
 *
 * Boundary: A20OS keeps no per-uid ownership on the ring, so the
 * read/is-this-my-kernel-buffer distinction Linux makes with -EPERM does not
 * exist here; every process reads the same messages.  There is no separate
 * console vs. ring severity split, so the CONSOLE_* actions only move the
 * single global klog_level.
 */
static int64_t syslog_read_ring(char *buf, int len, int clear_after)
{
    /* klog_read() fills a kernel buffer, so the payload is staged and handed
     * over in bounded chunks through copy_to_user(). */
    char tmp[256];
    size_t pos = 0;
    int64_t total = 0;
    while ((size_t)total < (size_t)len) {
        size_t want = (size_t)len - (size_t)total;
        if (want > sizeof(tmp)) want = sizeof(tmp);
        size_t cur = pos;
        int n = klog_read(tmp, want, &cur);
        if (n <= 0)
            break;
        if (copy_to_user(buf + total, tmp, (size_t)n) < 0)
            return -EFAULT;
        pos = cur;
        total += n;
    }
    if (clear_after)
        klog_clear();
    return total;
}

int64_t sys_syslog(int type, char *buf, int len) {
    switch (type) {
    case LINUX_SYSLOG_ACTION_SIZE_BUFFER:
        /* Linux returns the buffer capacity, not the bytes currently held. */
        return (int64_t)KLOG_BUF_SIZE;
    case LINUX_SYSLOG_ACTION_CLEAR:
        klog_clear();
        return 0;
    case LINUX_SYSLOG_ACTION_READ_ALL:
    case LINUX_SYSLOG_ACTION_READ_CLEAR:
        if (len < 0) return -EINVAL;
        if (len == 0) return 0;
        if (!buf) return -EFAULT;
        return syslog_read_ring(buf, len,
                                type == LINUX_SYSLOG_ACTION_READ_CLEAR);
    case LINUX_SYSLOG_ACTION_CONSOLE_OFF:
        /* klog_level is a lower bound: nothing is emitted at all once it
         * sits above KLOG_ERR. */
        klog_level = KLOG_ERR + 1;
        return 0;
    case LINUX_SYSLOG_ACTION_CONSOLE_LEVEL: {
        /* A20OS's klog_level is inverted with respect to Linux's
         * console_loglevel (higher klog_level = quieter), so the Linux level
         * is mirrored.  Only the KLOG_* severities the ring actually uses are
         * accepted; wider values are refused rather than silently clamped. */
        if (len < 0 || len > KLOG_ERR) return -EINVAL;
        klog_level = KLOG_ERR - len;
        return 0;
    }
    case LINUX_SYSLOG_ACTION_CLEAR_BOOT:
        /* The ring has no boot-time boundary to preserve. */
        return -EOPNOTSUPP;
    default:
        return -EINVAL;
    }
}

/* ============================================================
 * Random / Misc
 * ============================================================ */

int64_t sys_getrandom(void *buf, size_t len, unsigned int flags) {
    if (flags & ~LINUX_GRND_SUPPORTED)
        return -EINVAL;
    if ((flags & (LINUX_GRND_RANDOM | LINUX_GRND_INSECURE)) ==
        (LINUX_GRND_RANDOM | LINUX_GRND_INSECURE))
        return -EINVAL;
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

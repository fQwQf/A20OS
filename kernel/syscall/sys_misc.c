#include "syscall_internal.h"
#include "core/version.h"

int64_t sys_uname(void *buf) {
    struct uname { char s[65],n[65],r[65],v[65],m[65],d[65]; };
    if (!buf) return -EFAULT;
    struct uname u;
    memset(&u, 0, sizeof(u));
    strcpy(u.s, "A20OS");
    strcpy(u.n, "AAAAAAAAAAAAAAAAAAAA");
    strcpy(u.r, VERSION);
    strcpy(u.v, "A20OS version " VERSION);
    strcpy(u.m, ARCH_NAME);
    if (copy_to_user(buf, &u, sizeof(u)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_sysinfo(void *info) {
    if (!info) return -EFAULT;
    uint64_t si[14]; /* 112 bytes */
    memset(si, 0, sizeof(si));
    si[0] = timer_get_ticks() / TICKS_PER_SEC;
    si[1] = 1;
    if (copy_to_user(info, si, sizeof(si)) < 0) return -EFAULT;
    return 0;
}

int64_t sys_getgroups(int size, int *list) {
    (void)size; (void)list;
    return 0;
}

int64_t sys_setgroups(size_t size, const int *list) {
    (void)size; (void)list;
    return 0;
}

int64_t sys_umask(int newmask) {
    task_t *t = proc_current();
    if (!t) return 022;
    int old = t->umask;
    t->umask = newmask & 0777;
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

int64_t sys_futex(int *uaddr, int op, int val, void *timeout, int *uaddr2, int val3) {
    (void)timeout; (void)uaddr2; (void)val3;
    if (!uaddr) return -EFAULT;
    int opc = op & 0x7F;
    if (opc == 0 || opc == 9) {
        int uval;
        if (copy_from_user(&uval, uaddr, sizeof(int)) < 0) return -EFAULT;
        if (uval != val) return -EAGAIN;
        proc_yield();
        return 0;
    } else if (opc == 1 || opc == 10) {
        return 1;
    }
    return -ENOSYS;
}

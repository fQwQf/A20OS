#include "proc/acct.h"

#include "core/consts.h"
#include "core/errno.h"
#include "core/fcntl.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timer.h"
#include "fs/vfs.h"
#include "proc/proc.h"

/*
 * Linux acct(2) record (struct acct, 64-bit wire layout, version 3).
 * All times are in jiffies (TICKS_PER_SEC).  A20OS keeps the file open as a
 * global VFS fd; writes go through vfs_write() and never recurse into the
 * exiting task's own fd table.
 */

#define ACCT_COMM_LEN  16
#define ACCT_VERSION   3

#define AFORK    0x01
#define ASU      0x02
#define ACORE    0x04
#define AXSIG    0x08
#define ACCT_BYTE_ORDER 0x80

/* comp_t: 16-bit little-endian "compressed" value (13-bit mantissa, 3-bit
 * exponent) as defined by the Linux userspace ABI (include/linux/acct.h).
 * The kernel encodes raw ticks through encode_comp_t() before writing. */
typedef uint16_t comp_t;

static comp_t acct_encode_comp(uint32_t v)
{
    int exp = 0;
    while (v >= 8192) {
        v >>= 3;
        exp++;
    }
    return (comp_t)((v & 0x1fff) | ((uint32_t)exp << 13));
}

/* struct acct (Linux 64-bit wire layout, sizeof == 64).  Uses comp_t for the
 * time/IO fields and both uid16/gid16 and uid32/gid32 slots. */
typedef struct acct_record {
    uint8_t  ac_flag;
    uint8_t  ac_version;
    uint16_t ac_uid16;
    uint16_t ac_gid16;
    uint16_t ac_tty;
    uint32_t ac_btime;
    comp_t   ac_utime;
    comp_t   ac_stime;
    comp_t   ac_etime;
    comp_t   ac_mem;
    comp_t   ac_io;
    comp_t   ac_rw;
    comp_t   ac_minflt;
    comp_t   ac_majflt;
    comp_t   ac_swaps;
    uint16_t ac_ahz;
    uint32_t ac_exitcode;
    char     ac_comm[ACCT_COMM_LEN + 1];
    uint8_t  ac_etime_hi;
    uint16_t ac_etime_lo;
    uint32_t ac_uid;
    uint32_t ac_gid;
} __attribute__((packed)) acct_record_t;

_Static_assert(sizeof(acct_record_t) == 64,
               "struct acct must match the Linux 64-bit wire layout");

static spinlock_t g_acct_lock = SPINLOCK_INIT;
static int g_acct_fd = -1;              /* global VFS fd of the accounting file */
static char g_acct_path[MAX_PATH_LEN];

int acct_enable(const char *path)
{
    if (!path)
        return acct_disable();

    int gfd = vfs_open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (gfd < 0)
        return gfd;

    unsigned long flags = spin_lock_irqsave(&g_acct_lock);
    if (g_acct_fd >= 0)
        vfs_close(g_acct_fd);
    g_acct_fd = gfd;
    strncpy(g_acct_path, path, sizeof(g_acct_path) - 1);
    g_acct_path[sizeof(g_acct_path) - 1] = '\0';
    spin_unlock_irqrestore(&g_acct_lock, flags);
    return 0;
}

int acct_disable(void)
{
    unsigned long flags = spin_lock_irqsave(&g_acct_lock);
    int fd = g_acct_fd;
    g_acct_fd = -1;
    g_acct_path[0] = '\0';
    spin_unlock_irqrestore(&g_acct_lock, flags);
    if (fd >= 0)
        vfs_close(fd);
    return 0;
}

void acct_task_exit(struct task_t *t)
{
    if (!t)
        return;

    unsigned long flags = spin_lock_irqsave(&g_acct_lock);
    int gfd = g_acct_fd;
    if (gfd < 0) {
        spin_unlock_irqrestore(&g_acct_lock, flags);
        return;
    }

    acct_record_t rec;
    memset(&rec, 0, sizeof(rec));
    rec.ac_flag = ACCT_BYTE_ORDER;
    rec.ac_version = ACCT_VERSION;
    rec.ac_uid16 = (uint16_t)(t->cred.uid & 0xffff);
    rec.ac_gid16 = (uint16_t)(t->cred.gid & 0xffff);
    rec.ac_btime = (uint32_t)t->exec_start / TICKS_PER_SEC;
    uint64_t now = timer_get_ticks();
    uint64_t utime = t->total_time;
    uint64_t stime = now > t->exec_start ? now - t->exec_start : 0;
    if (stime > utime)
        stime -= utime;
    else
        stime = 0;
    rec.ac_utime = acct_encode_comp((uint32_t)utime);
    rec.ac_stime = acct_encode_comp((uint32_t)stime);
    uint64_t etime = now > t->exec_start ? now - t->exec_start : 0;
    rec.ac_etime = acct_encode_comp((uint32_t)etime);
    rec.ac_etime_hi = (uint8_t)(etime >> 32);
    rec.ac_etime_lo = (uint16_t)(etime >> 16);
    rec.ac_ahz = (uint16_t)TICKS_PER_SEC;
    rec.ac_exitcode = (uint32_t)(t->exit_code & 0xffff);
    rec.ac_uid = (uint32_t)t->cred.uid;
    rec.ac_gid = (uint32_t)t->cred.gid;
    strncpy(rec.ac_comm, t->name, ACCT_COMM_LEN);
    rec.ac_comm[ACCT_COMM_LEN] = '\0';

    (void)vfs_write(gfd, (const char *)&rec, sizeof(rec));
    (void)vfs_fsync(gfd);
    spin_unlock_irqrestore(&g_acct_lock, flags);
}

#include "fs/devfs.h"
#include "fs/file.h"
#include "mm/mm.h"
#include "core/timekeeping.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/random.h"
#include "core/sync.h"
#include "proc/proc.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "mm/slab.h"

extern void uart_putc(char c);
extern int  uart_getc(void);

#define TTY_LINE_SLOTS 16
#define TTY_LINE_BUF_SIZE 256

typedef struct tty_line_buffer {
    int pid;
    size_t len;
    char data[TTY_LINE_BUF_SIZE];
} tty_line_buffer_t;

static mutex_t g_tty_write_lock = MUTEX_INIT;
static int g_tty_line_owner = -1;
static tty_line_buffer_t g_tty_line_buffers[TTY_LINE_SLOTS];

enum {
    DEVFS_ROOT,
    DEVFS_MISC,
    DEVFS_NULL,
    DEVFS_ZERO,
    DEVFS_RANDOM,
    DEVFS_TTY,
    DEVFS_RTC,
    DEVFS_LOOP,
    DEVFS_LOOP_CTRL,
    DEVFS_PTMX,
    DEVFS_PTS_DIR,
    DEVFS_PTS,
    DEVFS_SHM_DIR,
    DEVFS_FB,
    DEVFS_INPUT,
    DEVFS_CLASS,
};

#define KTTY_NCCS 19

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t  c_line;
    uint8_t  c_cc[KTTY_NCCS];
} ktty_termios_t;

typedef struct {
    uint16_t ws_row;
    uint16_t ws_col;
    uint16_t ws_xpixel;
    uint16_t ws_ypixel;
} kwinsize_t;

typedef struct {
    ktty_termios_t termios;
    kwinsize_t winsize;
} tty_state_t;

typedef struct {
    int kind;
    const char *name;
    uint64_t rdev;
    class_device_t *class_dev;
    int dynamic;
} devfs_node_t;

static int devfs_lookup(vnode_t *dir, const char *name, vnode_t **out);
static int devfs_stat(vnode_t *vn, kstat_t *st);
static void devfs_release(vnode_t *vn);
static vfile_t *devfs_open_vnode(vnode_t *vn, int flags);

static vnode_ops_t g_devfs_ops = {
    .lookup = devfs_lookup,
    .stat = devfs_stat,
    .open = devfs_open_vnode,
    .release = devfs_release,
};

#define STATIC_NODE(k, n, d) { .kind = (k), .name = (n), .rdev = (d) }
static devfs_node_t g_nodes[] = {
    STATIC_NODE(DEVFS_ROOT, "", 0),
    STATIC_NODE(DEVFS_MISC, "misc", 0),
    STATIC_NODE(DEVFS_NULL, "null", 0x103),
    STATIC_NODE(DEVFS_ZERO, "zero", 0x105),
    STATIC_NODE(DEVFS_RANDOM, "random", 0x108),
    STATIC_NODE(DEVFS_RANDOM, "urandom", 0x109),
    STATIC_NODE(DEVFS_NULL, "cpu_dma_latency", 0x10a),
    STATIC_NODE(DEVFS_TTY, "tty", 0x500),
    STATIC_NODE(DEVFS_TTY, "console", 0x501),
    STATIC_NODE(DEVFS_RTC, "rtc", 0xfe00),
    STATIC_NODE(DEVFS_RTC, "rtc0", 0xfe00),
    STATIC_NODE(DEVFS_LOOP, "loop0", 0x700),
    STATIC_NODE(DEVFS_LOOP, "loop1", 0x701),
    STATIC_NODE(DEVFS_LOOP, "loop2", 0x702),
    STATIC_NODE(DEVFS_FB, "fb0", 0x1d00),
    STATIC_NODE(DEVFS_INPUT, "event0", 0x1d01),
    STATIC_NODE(DEVFS_LOOP, "loop3", 0x703),
    STATIC_NODE(DEVFS_LOOP, "loop4", 0x704),
    STATIC_NODE(DEVFS_LOOP, "loop5", 0x705),
    STATIC_NODE(DEVFS_LOOP, "loop6", 0x706),
    STATIC_NODE(DEVFS_LOOP, "loop7", 0x707),
    STATIC_NODE(DEVFS_LOOP_CTRL, "loop-control", 0x70c),
    STATIC_NODE(DEVFS_PTMX, "ptmx", 0x502),
    STATIC_NODE(DEVFS_PTS_DIR, "pts", 0),
    STATIC_NODE(DEVFS_PTS, "0", 0x8000),
    STATIC_NODE(DEVFS_PTS, "1", 0x8001),
    STATIC_NODE(DEVFS_PTS, "2", 0x8002),
    STATIC_NODE(DEVFS_PTS, "3", 0x8003),
    STATIC_NODE(DEVFS_PTS, "4", 0x8004),
    STATIC_NODE(DEVFS_PTS, "5", 0x8005),
    STATIC_NODE(DEVFS_PTS, "6", 0x8006),
    STATIC_NODE(DEVFS_PTS, "7", 0x8007),
    STATIC_NODE(DEVFS_SHM_DIR, "shm", 0),
};

static vnode_t g_vnodes[sizeof(g_nodes) / sizeof(g_nodes[0])];
static tty_state_t g_dev_tty;

static void fill_default_termios(ktty_termios_t *tio) {
    memset(tio, 0, sizeof(*tio));
    tio->c_iflag = 0x500;
    tio->c_oflag = 0x5;
    tio->c_cflag = 0xBF;
    tio->c_lflag = 0x8a3b;
    tio->c_cc[0] = 3;
    tio->c_cc[1] = 28;
    tio->c_cc[2] = 127;
    tio->c_cc[3] = 21;
    tio->c_cc[4] = 4;
    tio->c_cc[5] = 0;
    tio->c_cc[6] = 1;
    tio->c_cc[8] = 17;
    tio->c_cc[9] = 19;
    tio->c_cc[10] = 26;
    tio->c_cc[12] = 18;
    tio->c_cc[13] = 15;
    tio->c_cc[14] = 23;
    tio->c_cc[15] = 22;
}

static void init_default_tty_state(tty_state_t *tty) {
    fill_default_termios(&tty->termios);
    tty->winsize.ws_row = 24;
    tty->winsize.ws_col = 80;
    tty->winsize.ws_xpixel = 0;
    tty->winsize.ws_ypixel = 0;
}

static int devfs_stdin_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    if (count == 0) return 0;
    int c = uart_getc();
    if (c < 0) return 0;
    if (c == '\r') c = '\n';
    buf[0] = (char)c;
    return 1;
}

static tty_line_buffer_t *tty_line_buffer_for(int pid) {
    tty_line_buffer_t *free_slot = NULL;
    for (int i = 0; i < TTY_LINE_SLOTS; i++) {
        if (g_tty_line_buffers[i].len > 0 && g_tty_line_buffers[i].pid == pid)
            return &g_tty_line_buffers[i];
        if (!free_slot && g_tty_line_buffers[i].len == 0)
            free_slot = &g_tty_line_buffers[i];
    }
    if (free_slot) {
        free_slot->pid = pid;
        free_slot->len = 0;
    }
    return free_slot;
}

static void tty_write_owned_char(int pid, char c) {
    if (g_tty_line_owner < 0)
        g_tty_line_owner = pid;
    uart_putc(c);
    if (c == '\n')
        g_tty_line_owner = -1;
}

static int tty_line_owner_live_locked(void) {
    if (g_tty_line_owner < 0)
        return 0;
    task_t *owner = proc_find_get(g_tty_line_owner);
    int live =
        owner && owner->state != PROC_ZOMBIE && owner->state != PROC_UNUSED;
    proc_put(owner);
    return live;
}

static void tty_drain_pending_locked(void) {
    int progress = 1;
    while (g_tty_line_owner < 0 && progress) {
        progress = 0;
        for (int i = 0; i < TTY_LINE_SLOTS; i++) {
            tty_line_buffer_t *b = &g_tty_line_buffers[i];
            if (b->len == 0) continue;
            int pid = b->pid;
            size_t n = b->len;
            char tmp[TTY_LINE_BUF_SIZE];
            memcpy(tmp, b->data, n);
            b->len = 0;
            progress = 1;
            for (size_t j = 0; j < n; j++)
                tty_write_owned_char(pid, tmp[j]);
            if (g_tty_line_owner >= 0)
                return;
        }
    }
}

static void tty_release_dead_owner_locked(void) {
    if (g_tty_line_owner >= 0 && !tty_line_owner_live_locked()) {
        g_tty_line_owner = -1;
        tty_drain_pending_locked();
    }
}

static void tty_buffer_pending_char(int pid, char c) {
    tty_line_buffer_t *b = tty_line_buffer_for(pid);
    if (!b) {
        uart_putc(c);
        if (c == '\n')
            g_tty_line_owner = -1;
        return;
    }
    if (b->len >= TTY_LINE_BUF_SIZE) {
        for (size_t i = 0; i < b->len; i++)
            uart_putc(b->data[i]);
        b->len = 0;
    }
    b->data[b->len++] = c;
}

static int devfs_stdout_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf;
    task_t *t = proc_current();
    int pid = t ? t->pid : -1;
    mutex_lock(&g_tty_write_lock);
    tty_release_dead_owner_locked();
    for (size_t i = 0; i < count; i++) {
        char c = buf[i];
        if (pid < 0 || g_tty_line_owner < 0 || g_tty_line_owner == pid) {
            tty_write_owned_char(pid, c);
            if (g_tty_line_owner < 0)
                tty_drain_pending_locked();
        } else {
            tty_buffer_pending_char(pid, c);
        }
    }
    mutex_unlock(&g_tty_write_lock);
    return (int)count;
}

static int devfs_null_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf; (void)buf; (void)count;
    return 0;
}

static int devfs_null_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf; (void)buf;
    return (int)count;
}

static int devfs_zero_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    memset(buf, 0, count);
    return (int)count;
}

static int devfs_random_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf;
    random_fill(buf, count);
    return (int)count;
}

static int devfs_dir_readdir(vfile_t *vf, void *dirp, size_t count) {
    static const struct {
        const char *name;
        uint8_t type;
    } root_entries[] = {
        { ".", DT_DIR }, { "..", DT_DIR }, { "misc", DT_DIR },
        { "null", DT_CHR }, { "zero", DT_CHR }, { "tty", DT_CHR },
        { "random", DT_CHR }, { "urandom", DT_CHR },
        { "cpu_dma_latency", DT_CHR },
        { "rtc", DT_CHR }, { "rtc0", DT_CHR },
        { "console", DT_CHR },
        { "loop0", DT_BLK }, { "loop1", DT_BLK },
        { "loop2", DT_BLK }, { "loop3", DT_BLK },
        { "loop4", DT_BLK }, { "loop5", DT_BLK },
        { "loop6", DT_BLK }, { "loop7", DT_BLK },
    { "loop-control", DT_CHR },
    { "ptmx", DT_CHR },
    { "pts", DT_DIR },
        { "shm", DT_DIR },
    };
    static const struct {
        const char *name;
        uint8_t type;
    } misc_entries[] = {
        { ".", DT_DIR }, { "..", DT_DIR }, { "rtc", DT_CHR },
    };
    static const struct {
        const char *name;
        uint8_t type;
    } pts_entries[] = {
        { ".", DT_DIR }, { "..", DT_DIR },
        { "0", DT_CHR }, { "1", DT_CHR }, { "2", DT_CHR }, { "3", DT_CHR },
        { "4", DT_CHR }, { "5", DT_CHR }, { "6", DT_CHR }, { "7", DT_CHR },
    };

    int kind = (int)(intptr_t)vf->priv;
    const void *entries_void = NULL;
    size_t nentries = 0;
    if (kind == DEVFS_ROOT) {
        entries_void = root_entries;
        nentries = sizeof(root_entries) / sizeof(root_entries[0]);
    } else if (kind == DEVFS_MISC) {
        entries_void = misc_entries;
        nentries = sizeof(misc_entries) / sizeof(misc_entries[0]);
    } else if (kind == DEVFS_PTS_DIR) {
        entries_void = pts_entries;
        nentries = sizeof(pts_entries) / sizeof(pts_entries[0]);
    } else {
        return -ENOTDIR;
    }

    const typeof(root_entries[0]) *entries = entries_void;
    size_t idx = vf->offset;
    size_t total = 0;
    char *out = (char *)dirp;
    while (idx < nentries) {
        size_t namelen = strlen(entries[idx].name);
        size_t reclen = (offsetof(vfs_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
        if (total + reclen > count)
            break;

        vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
        d->d_ino = idx + 1;
        d->d_off = (int64_t)(idx + 1);
        d->d_reclen = (uint16_t)reclen;
        d->d_type = entries[idx].type;
        memcpy(d->d_name, entries[idx].name, namelen + 1);
        total += reclen;
        idx++;
    }

    if (kind == DEVFS_ROOT && idx >= nentries) {
        unsigned visible = (unsigned)(idx - nentries);
        unsigned ordinal = 0;
        for (;;) {
            class_device_t *cdev = class_device_get_nth(ordinal++);
            if (!cdev)
                break;
            if (!class_device_has_devnode(cdev)) {
                class_device_put(cdev);
                continue;
            }
            if (visible > 0) {
                visible--;
                class_device_put(cdev);
                continue;
            }
            size_t namelen = strlen(cdev->name);
            size_t reclen = (offsetof(vfs_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
            if (total + reclen > count) {
                class_device_put(cdev);
                break;
            }
            vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
            d->d_ino = 0x10000U + cdev->index;
            d->d_off = (int64_t)(idx + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = class_device_dirent_type(cdev);
            memcpy(d->d_name, cdev->name, namelen + 1);
            total += reclen;
            idx++;
            class_device_put(cdev);
        }
    }
    vf->offset = idx;
    return (int)total;
}

#define RTC_RD_TIME    0x80247009UL
#define RTC_SET_TIME   0x4024700aUL
#define RTC_IRQP_READ  0x8008700bUL
#define RTC_EPOCH_READ 0x8008700dUL
#define TCGETS         0x5401
#define TCSETS         0x5402
#define TCSETSW        0x5403
#define TCSETSF        0x5404
#define TIOCGWINSZ     0x5413

typedef struct {
    int32_t tm_sec;
    int32_t tm_min;
    int32_t tm_hour;
    int32_t tm_mday;
    int32_t tm_mon;
    int32_t tm_year;
    int32_t tm_wday;
    int32_t tm_yday;
    int32_t tm_isdst;
} krtc_time_t;

static int is_leap_year(int year) {
    return (year % 4 == 0) && ((year % 100) != 0 || (year % 400) == 0);
}

static int rtc_time_to_unix_seconds(const krtc_time_t *rt, uint64_t *secs) {
    static const int month_days[12] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    int year = rt->tm_year + 1900;
    int month = rt->tm_mon + 1;
    int hour = rt->tm_hour;
    int min = rt->tm_min;
    int sec = rt->tm_sec;
    int mday = rt->tm_mday;
    int mdays;
    int64_t z;
    int64_t era;
    unsigned yoe;
    unsigned doy;
    unsigned doe;

    if (year < 1970 || month < 1 || month > 12 || mday < 1 ||
        hour < 0 || hour > 23 || min < 0 || min > 59 || sec < 0 || sec > 59)
        return -EINVAL;

    mdays = month_days[month - 1];
    if (month == 2 && is_leap_year(year)) mdays = 29;
    if (mday > mdays) return -EINVAL;

    year -= month <= 2;
    era = (year >= 0 ? year : year - 399) / 400;
    yoe = (unsigned)(year - era * 400);
    doy = (153U * (unsigned)(month + (month > 2 ? -3 : 9)) + 2U) / 5U + (unsigned)mday - 1U;
    doe = yoe * 365U + yoe / 4U - yoe / 100U + doy;
    z = era * 146097 + (int64_t)doe - 719468;
    if (z < 0) return -EINVAL;

    *secs = (uint64_t)z * 86400ULL + (uint64_t)hour * 3600ULL +
            (uint64_t)min * 60ULL + (uint64_t)sec;
    return 0;
}

static void unix_seconds_to_rtc_time(uint64_t secs, krtc_time_t *rt) {
    static const int month_yday[2][12] = {
        { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 },
        { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 },
    };
    uint64_t days = secs / 86400ULL;
    uint64_t rem = secs % 86400ULL;
    int64_t z = (int64_t)days + 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int year = (int)(yoe + era * 400);
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    unsigned mday = doy - (153 * mp + 2) / 5 + 1;
    unsigned month = mp + (mp < 10 ? 3 : (unsigned)-9);

    year += (month <= 2);
    memset(rt, 0, sizeof(*rt));
    rt->tm_sec = (int32_t)(rem % 60ULL);
    rem /= 60ULL;
    rt->tm_min = (int32_t)(rem % 60ULL);
    rem /= 60ULL;
    rt->tm_hour = (int32_t)rem;
    rt->tm_mday = (int32_t)mday;
    rt->tm_mon = (int32_t)(month - 1);
    rt->tm_year = (int32_t)(year - 1900);
    rt->tm_wday = (int32_t)((days + 4ULL) % 7ULL);
    rt->tm_yday = month_yday[is_leap_year(year)][month - 1] + (int)mday - 1;
    rt->tm_isdst = 0;
}

static int devfs_rtc_ioctl(unsigned long req, void *arg) {
    if ((req == RTC_RD_TIME || req == RTC_IRQP_READ || req == RTC_EPOCH_READ) && !arg)
        return -EFAULT;

    switch (req) {
    case RTC_RD_TIME: {
        krtc_time_t tm;
        uint64_t ts[2];
        timekeeping_get_realtime(ts);
        unix_seconds_to_rtc_time(ts[0], &tm);
        if (copy_to_user(arg, &tm, sizeof(tm)) < 0) return -EFAULT;
        return 0;
    }
    case RTC_IRQP_READ: {
        unsigned long irqp = 1;
        if (copy_to_user(arg, &irqp, sizeof(irqp)) < 0) return -EFAULT;
        return 0;
    }
    case RTC_EPOCH_READ: {
        unsigned long epoch = 1900;
        if (copy_to_user(arg, &epoch, sizeof(epoch)) < 0) return -EFAULT;
        return 0;
    }
    case RTC_SET_TIME: {
        krtc_time_t tm;
        uint64_t secs;
        if (copy_from_user(&tm, arg, sizeof(tm)) < 0) return -EFAULT;
        if (rtc_time_to_unix_seconds(&tm, &secs) < 0) return -EINVAL;
        return timekeeping_set_realtime(secs, 0);
    }
    default:
        return -ENOTTY;
    }
}

static int devfs_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    int kind = (int)(intptr_t)vf->priv;
    if (kind == DEVFS_TTY) {
        if (!arg) return -EFAULT;
        if (req == TCGETS) {
            if (copy_to_user(arg, &g_dev_tty.termios, sizeof(g_dev_tty.termios)) < 0) return -EFAULT;
            return 0;
        }
        if (req == TCSETS || req == TCSETSW || req == TCSETSF) {
            ktty_termios_t tio;
            if (copy_from_user(&tio, arg, sizeof(tio)) < 0) return -EFAULT;
            g_dev_tty.termios = tio;
            return 0;
        }
        if (req == TIOCGWINSZ) {
            if (copy_to_user(arg, &g_dev_tty.winsize, sizeof(g_dev_tty.winsize)) < 0) return -EFAULT;
            return 0;
        }
        return -ENOTTY;
    }
    if (kind == DEVFS_RTC)
        return devfs_rtc_ioctl(req, arg);
    return -ENOTTY;
}

extern int  loop_dev_read(int idx, char *buf, size_t count, size_t offset);
extern int  loop_dev_write(int idx, const char *buf, size_t count, size_t offset);
extern int  loop_dev_ioctl(vfile_t *vf, unsigned long req, void *arg);
extern int  loop_control_ioctl(unsigned long req, void *arg);
extern int  pty_alloc_and_open(void);
extern int  pty_master_read(int idx, char *buf, size_t count);
extern int  pty_master_write(int idx, const char *buf, size_t count);
extern int  pty_master_ioctl(int idx, unsigned long req, void *arg);
extern void pty_master_close(int idx);
extern int  pty_slave_read(int idx, char *buf, size_t count);
extern int  pty_slave_write(int idx, const char *buf, size_t count);
extern int  pty_slave_ioctl(int idx, unsigned long req, void *arg);
extern void pty_slave_close(int idx);
extern int  pty_slave_open(int idx);
extern void pty_init(void);

static int devfs_loop_read(vfile_t *vf, char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv - 100;
    size_t off = vf->offset;
    int r = loop_dev_read(idx, buf, count, off);
    if (r > 0) vf->offset += r;
    return r;
}

static int devfs_loop_write(vfile_t *vf, const char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv - 100;
    size_t off = vf->offset;
    int r = loop_dev_write(idx, buf, count, off);
    if (r > 0) vf->offset += r;
    return r;
}

static long devfs_noop_lseek(vfile_t *vf, long offset, int whence) {
    if (whence == SEEK_SET) vf->offset = (size_t)offset;
    else if (whence == SEEK_CUR) vf->offset += (size_t)offset;
    return (long)vf->offset;
}

static vfile_ops_t g_devfs_loop_ops  = { .read = devfs_loop_read, .write = devfs_loop_write, .lseek = devfs_noop_lseek, .ioctl = loop_dev_ioctl };

static int devfs_loop_control_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    (void)vf;
    return loop_control_ioctl(req, arg);
}
static int devfs_loop_control_read(vfile_t *vf, char *buf, size_t count) {
    (void)vf; (void)buf; (void)count;
    return -EINVAL;
}
static vfile_ops_t g_devfs_loop_ctrl_ops = { .read = devfs_loop_control_read, .ioctl = devfs_loop_control_ioctl };

static int devfs_ptm_read(vfile_t *vf, char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_master_read(idx, buf, count);
}
static int devfs_ptm_write(vfile_t *vf, const char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_master_write(idx, buf, count);
}
static int devfs_ptm_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_master_ioctl(idx, req, arg);
}
static int devfs_ptm_close(vfile_t *vf) {
    int idx = (int)(intptr_t)vf->priv;
    pty_master_close(idx);
    return 0;
}
static vfile_ops_t g_devfs_ptm_ops = { .read = devfs_ptm_read, .write = devfs_ptm_write, .ioctl = devfs_ptm_ioctl, .close = devfs_ptm_close };

static int devfs_pts_read(vfile_t *vf, char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_slave_read(idx, buf, count);
}
static int devfs_pts_write(vfile_t *vf, const char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_slave_write(idx, buf, count);
}
static int devfs_pts_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_slave_ioctl(idx, req, arg);
}
static int devfs_pts_close(vfile_t *vf) {
    int idx = (int)(intptr_t)vf->priv;
    pty_slave_close(idx);
    return 0;
}
static vfile_ops_t g_devfs_pts_ops = { .read = devfs_pts_read, .write = devfs_pts_write, .ioctl = devfs_pts_ioctl, .close = devfs_pts_close };

static vfile_ops_t g_devfs_tty_ops    = { .read = devfs_stdin_read, .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_dir_ops    = { .read = devfs_null_read,  .readdir = devfs_dir_readdir, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stdin_ops  = { .read = devfs_stdin_read, .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stdout_ops = { .read = devfs_null_read,  .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stderr_ops = { .read = devfs_null_read,  .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_null_ops   = { .read = devfs_null_read,  .write = devfs_null_write,   .lseek = devfs_noop_lseek, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_zero_ops   = { .read = devfs_zero_read,  .write = devfs_null_write,   .lseek = devfs_noop_lseek, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_random_ops = { .read = devfs_random_read,.write = devfs_null_write,   .lseek = devfs_noop_lseek, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_rtc_ops    = { .read = devfs_null_read,  .write = devfs_null_write,   .lseek = devfs_noop_lseek, .ioctl = devfs_ioctl };

static int devfs_class_read(vfile_t *vf, char *buf, size_t count)
{
    class_device_t *cdev = vf ? (class_device_t *)vf->priv : NULL;
    if (class_device_call_begin(cdev) < 0)
        return -ENODEV;
    device_t *dev = cdev->dev;
    int ret = -ENOSYS;
    if (cdev->class_type == DEV_CLASS_BLOCK) {
        const block_dev_ops_t *ops = dev->drv->class_ops;
        uint32_t sector = ops->sector_size ? ops->sector_size(dev) : 512;
        if (!sector || (vf->offset % sector) || (count % sector))
            ret = -EINVAL;
        else if (ops->read) {
            ret = ops->read(dev, vf->offset / sector, buf, count / sector);
            if (ret == 0) {
                vf->offset += count;
                ret = (int)count;
            }
        }
    } else if (cdev->class_type == DEV_CLASS_CHAR) {
        const char_dev_ops_t *ops = dev->drv->class_ops;
        if (ops->read) ret = ops->read(dev, buf, count);
    } else if (cdev->class_type == DEV_CLASS_AUDIO) {
        const audio_dev_ops_t *ops = dev->drv->class_ops;
        if (ops->read) ret = ops->read(dev, buf, count);
    }
    class_device_call_end(cdev);
    return ret;
}

static int devfs_class_write(vfile_t *vf, const char *buf, size_t count)
{
    class_device_t *cdev = vf ? (class_device_t *)vf->priv : NULL;
    if (class_device_call_begin(cdev) < 0)
        return -ENODEV;
    device_t *dev = cdev->dev;
    int ret = -ENOSYS;
    if (cdev->class_type == DEV_CLASS_BLOCK) {
        const block_dev_ops_t *ops = dev->drv->class_ops;
        uint32_t sector = ops->sector_size ? ops->sector_size(dev) : 512;
        if (!sector || (vf->offset % sector) || (count % sector))
            ret = -EINVAL;
        else if (ops->write) {
            ret = ops->write(dev, vf->offset / sector, buf, count / sector);
            if (ret == 0) {
                vf->offset += count;
                ret = (int)count;
            }
        }
    } else if (cdev->class_type == DEV_CLASS_CHAR) {
        const char_dev_ops_t *ops = dev->drv->class_ops;
        if (ops->write) ret = ops->write(dev, buf, count);
    } else if (cdev->class_type == DEV_CLASS_AUDIO) {
        const audio_dev_ops_t *ops = dev->drv->class_ops;
        if (ops->write) ret = ops->write(dev, buf, count);
    }
    class_device_call_end(cdev);
    return ret;
}

static int devfs_class_ioctl(vfile_t *vf, unsigned long req, void *arg)
{
    class_device_t *cdev = vf ? (class_device_t *)vf->priv : NULL;
    if (class_device_call_begin(cdev) < 0)
        return -ENODEV;
    device_t *dev = cdev->dev;
    int ret = -ENOTTY;
    if (cdev->class_type == DEV_CLASS_BLOCK) {
        const block_dev_ops_t *ops = dev->drv->class_ops;
        if (req == BLK_IOCTL_GET_CAPACITY && ops->capacity && arg) {
            uint64_t capacity = ops->capacity(dev);
            ret = copy_to_user(arg, &capacity, sizeof(capacity)) < 0 ?
                  -EFAULT : 0;
        } else if (req == BLK_IOCTL_GET_SECTOR_SZ && ops->sector_size && arg) {
            uint32_t sector = ops->sector_size(dev);
            ret = copy_to_user(arg, &sector, sizeof(sector)) < 0 ?
                  -EFAULT : 0;
        } else if (req == BLK_IOCTL_SYNC && ops->flush) {
            ret = ops->flush(dev);
        } else if (ops->ioctl) {
            ret = ops->ioctl(dev, req, arg);
        }
    } else if (cdev->class_type == DEV_CLASS_CHAR) {
        const char_dev_ops_t *ops = dev->drv->class_ops;
        if (ops->ioctl) ret = ops->ioctl(dev, req, arg);
    } else if (cdev->class_type == DEV_CLASS_AUDIO) {
        const audio_dev_ops_t *ops = dev->drv->class_ops;
        if (ops->ioctl) ret = ops->ioctl(dev, req, arg);
    }
    class_device_call_end(cdev);
    return ret;
}

static long devfs_class_lseek(vfile_t *vf, long offset, int whence)
{
    class_device_t *cdev = vf ? (class_device_t *)vf->priv : NULL;
    if (!cdev || cdev->class_type != DEV_CLASS_BLOCK)
        return -ESPIPE;
    if (class_device_call_begin(cdev) < 0)
        return -ENODEV;
    device_t *dev = cdev->dev;
    const block_dev_ops_t *ops = dev->drv->class_ops;
    uint32_t sector = ops->sector_size ? ops->sector_size(dev) : 512;
    uint64_t sectors = ops->capacity ? ops->capacity(dev) : 0;
    size_t end = 0;
    int invalid = !sector || sectors > (uint64_t)(~(size_t)0) / sector;
    if (!invalid)
        end = (size_t)sectors * sector;
    size_t base = 0;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = vf->offset;
    else if (whence == SEEK_END) base = end;
    else invalid = 1;
    size_t next = base;
    if (!invalid && offset >= 0) {
        if ((size_t)offset > ~(size_t)0 - base) invalid = 1;
        else next = base + (size_t)offset;
    } else if (!invalid) {
        size_t magnitude = (size_t)(-(offset + 1L)) + 1U;
        if (magnitude > base) invalid = 1;
        else next = base - magnitude;
    }
    if (!invalid && next > end)
        invalid = 1;
    if (!invalid)
        vf->offset = next;
    class_device_call_end(cdev);
    return invalid ? -EINVAL : (long)next;
}

static int devfs_class_close(vfile_t *vf)
{
    if (vf && vf->priv)
        class_device_put((class_device_t *)vf->priv);
    return 0;
}

static vfile_ops_t g_devfs_class_ops = {
    .read = devfs_class_read,
    .write = devfs_class_write,
    .lseek = devfs_class_lseek,
    .ioctl = devfs_class_ioctl,
    .close = devfs_class_close,
};

static vfile_t g_stdin_file  = { .ref_count = REFCOUNT_INIT(999), .ops = &g_devfs_stdin_ops,  .flags = O_RDONLY, .priv = (void *)(intptr_t)DEVFS_TTY };
static vfile_t g_stdout_file = { .ref_count = REFCOUNT_INIT(999), .ops = &g_devfs_stdout_ops, .flags = O_WRONLY, .priv = (void *)(intptr_t)DEVFS_TTY };
static vfile_t g_stderr_file = { .ref_count = REFCOUNT_INIT(999), .ops = &g_devfs_stderr_ops, .flags = O_WRONLY, .priv = (void *)(intptr_t)DEVFS_TTY };

static vnode_t *node_to_vnode(size_t idx) {
    vnode_get(&g_vnodes[idx]);
    return &g_vnodes[idx];
}

static int devfs_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    devfs_node_t *node = (devfs_node_t *)dir->fs_data;
    if (!node || !out) return -ENOENT;
    *out = NULL;

    if (node->kind == DEVFS_ROOT) {
        for (size_t i = 1; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
            if (strcmp(name, g_nodes[i].name) == 0) {
                *out = node_to_vnode(i);
                return 0;
            }
        }
        class_device_t *cdev = class_device_get_by_name(name);
        if (cdev && class_device_has_devnode(cdev)) {
            devfs_node_t *dynamic = kcalloc(1, sizeof(*dynamic));
            vnode_t *vn = kcalloc(1, sizeof(*vn));
            if (!dynamic || !vn) {
                kfree(dynamic);
                kfree(vn);
                class_device_put(cdev);
                return -ENOMEM;
            }
            dynamic->kind = DEVFS_CLASS;
            dynamic->name = cdev->name;
            dynamic->rdev = cdev->devt;
            dynamic->class_dev = cdev;
            dynamic->dynamic = 1;
            vn->ino = 0x10000U + ((uint64_t)cdev->class_type << 8) +
                      cdev->index;
            vn->type = VFS_FT_REGULAR;
            vn->mode = (cdev->class_type == DEV_CLASS_BLOCK ? S_IFBLK : S_IFCHR) | 0660;
            vnode_ref_init(vn, 1);
            vn->parent = dir;
            vnode_get(dir);
            vn->fs_data = dynamic;
            vn->ops = &g_devfs_ops;
            *out = vn;
            return 0;
        }
        class_device_put(cdev);
    } else if (node->kind == DEVFS_MISC && strcmp(name, "rtc") == 0) {
        for (size_t i = 1; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
            if (g_nodes[i].kind == DEVFS_RTC && strcmp(g_nodes[i].name, "rtc") == 0) {
                *out = node_to_vnode(i);
                return 0;
            }
        }
    } else if (node->kind == DEVFS_PTS_DIR) {
        for (size_t i = 1; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
            if (g_nodes[i].kind == DEVFS_PTS && strcmp(name, g_nodes[i].name) == 0) {
                *out = node_to_vnode(i);
                return 0;
            }
        }
    }
    return -ENOENT;
}

static void fill_char_kstat(kstat_t *st, uint64_t rdev) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_rdev = rdev;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

static int devfs_stat(vnode_t *vn, kstat_t *st) {
    devfs_node_t *node = (devfs_node_t *)vn->fs_data;
    if (!node || !st) return -EINVAL;
    memset(st, 0, sizeof(*st));
    if (node->kind == DEVFS_ROOT || node->kind == DEVFS_MISC ||
        node->kind == DEVFS_SHM_DIR || node->kind == DEVFS_PTS_DIR) {
        st->st_mode = S_IFDIR | 0555;
        st->st_uid = 0;
        st->st_gid = 0;
        st->st_nlink = 2;
        st->st_blksize = 4096;
    } else if (node->kind == DEVFS_CLASS && node->class_dev) {
        st->st_mode = (node->class_dev->class_type == DEV_CLASS_BLOCK ?
                       S_IFBLK : S_IFCHR) | 0660;
        st->st_rdev = node->rdev;
        st->st_uid = 0;
        st->st_gid = 0;
        st->st_nlink = 1;
        st->st_blksize = 4096;
    } else if (node->kind == DEVFS_LOOP) {
        st->st_mode = S_IFBLK | 0660;
        st->st_rdev = node->rdev;
        st->st_uid = 0;
        st->st_gid = 0;
        st->st_nlink = 1;
        st->st_blksize = 4096;
    } else if (node->kind == DEVFS_LOOP_CTRL) {
        fill_char_kstat(st, node->rdev);
    } else if (node->kind == DEVFS_PTMX) {
        fill_char_kstat(st, node->rdev);
    } else if (node->kind == DEVFS_PTS) {
        fill_char_kstat(st, node->rdev);
    } else {
        fill_char_kstat(st, node->rdev);
    }
    return 0;
}

static void devfs_release(vnode_t *vn) {
    devfs_node_t *node = vn ? (devfs_node_t *)vn->fs_data : NULL;
    if (!node || !node->dynamic)
        return;
    if (vn->parent)
        vnode_put(vn->parent);
    class_device_put(node->class_dev);
    kfree(node);
    kfree(vn);
}

static vfile_t *devfs_open_vnode(vnode_t *vn, int flags) {
    devfs_node_t *node = vn ? (devfs_node_t *)vn->fs_data : NULL;
    if (!node)
        return NULL;

    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags;
    vfile_ref_init(vf, 1);
    vf->priv = (void *)(intptr_t)node->kind;

    switch (node->kind) {
    case DEVFS_ROOT:
    case DEVFS_MISC: vf->ops = &g_devfs_dir_ops; break;
    case DEVFS_NULL: vf->ops = &g_devfs_null_ops; break;
    case DEVFS_ZERO: vf->ops = &g_devfs_zero_ops; break;
    case DEVFS_RANDOM: vf->ops = &g_devfs_random_ops; break;
    case DEVFS_TTY:  vf->ops = &g_devfs_tty_ops; break;
    case DEVFS_RTC:  vf->ops = &g_devfs_rtc_ops; break;
    case DEVFS_FB:   vf->ops = &g_devfs_fb_ops; break;
    case DEVFS_INPUT: vf->ops = &g_devfs_input_ops; break;
    case DEVFS_CLASS:
        if (!node->class_dev || !__atomic_load_n(&node->class_dev->online,
                                                 __ATOMIC_ACQUIRE)) {
            vnode_put(vf->vnode);
            vfile_free(vf);
            return NULL;
        }
        class_device_get(node->class_dev);
        vf->ops = &g_devfs_class_ops;
        vf->priv = node->class_dev;
        break;
    case DEVFS_LOOP: {
        int loop_idx = (int)(node->rdev & 0xFF);
        vf->ops = &g_devfs_loop_ops;
        vf->priv = (void *)(intptr_t)(loop_idx + 100);
        break;
    }
    case DEVFS_LOOP_CTRL:
        vf->ops = &g_devfs_loop_ctrl_ops;
        break;
    case DEVFS_PTMX: {
        int pty_idx = pty_alloc_and_open();
        if (pty_idx < 0) {
            vfile_free(vf);
            return NULL;
        }
        vf->ops = &g_devfs_ptm_ops;
        vf->priv = (void *)(intptr_t)pty_idx;
        break;
    }
    case DEVFS_PTS_DIR:
        vf->ops = &g_devfs_dir_ops;
        break;
    case DEVFS_PTS: {
        int pts_idx = (int)(node->rdev & 0xFF);
        int result = pty_slave_open(pts_idx);
        if (result < 0) {
            vnode_put(vf->vnode);
            vfile_free(vf);
            return NULL;
        }
        vf->ops = &g_devfs_pts_ops;
        vf->priv = (void *)(intptr_t)pts_idx;
        break;
    }
    default:
        vfile_free(vf);
        return NULL;
    }
    return vf;
}

int devfs_is_char_vfile(vfile_t *vf) {
    return vf && (vf->ops == &g_devfs_null_ops ||
                  vf->ops == &g_devfs_zero_ops ||
                  vf->ops == &g_devfs_random_ops ||
                  vf->ops == &g_devfs_tty_ops ||
                  vf->ops == &g_devfs_stdin_ops ||
                  vf->ops == &g_devfs_stdout_ops ||
                  vf->ops == &g_devfs_stderr_ops ||
                  vf->ops == &g_devfs_rtc_ops ||
                  vf->ops == &g_devfs_class_ops);
}

int devfs_is_tty_vfile(vfile_t *vf) {
    return vf && (vf->ops == &g_devfs_tty_ops ||
                  vf->ops == &g_devfs_stdin_ops ||
                  vf->ops == &g_devfs_stdout_ops ||
                  vf->ops == &g_devfs_stderr_ops);
}

int devfs_is_zero_vfile(vfile_t *vf) {
    return vf && vf->ops == &g_devfs_zero_ops;
}


vfile_t *devfs_create_stdio(int fd) {
    if (fd == STDIN_FILENO) return &g_stdin_file;
    if (fd == STDOUT_FILENO) return &g_stdout_file;
    if (fd == STDERR_FILENO) return &g_stderr_file;
    return NULL;
}

vnode_t *devfs_mount(void) {
    init_default_tty_state(&g_dev_tty);
    memset(&g_stdin_file, 0, sizeof(g_stdin_file));
    refcount_set(&g_stdin_file.ref_count, 999);
    g_stdin_file.ops = &g_devfs_stdin_ops;
    g_stdin_file.flags = O_RDONLY;
    g_stdin_file.priv = (void *)(intptr_t)DEVFS_TTY;
    memset(&g_stdout_file, 0, sizeof(g_stdout_file));
    refcount_set(&g_stdout_file.ref_count, 999);
    g_stdout_file.ops = &g_devfs_stdout_ops;
    g_stdout_file.flags = O_WRONLY;
    g_stdout_file.priv = (void *)(intptr_t)DEVFS_TTY;
    memset(&g_stderr_file, 0, sizeof(g_stderr_file));
    refcount_set(&g_stderr_file.ref_count, 999);
    g_stderr_file.ops = &g_devfs_stderr_ops;
    g_stderr_file.flags = O_WRONLY;
    g_stderr_file.priv = (void *)(intptr_t)DEVFS_TTY;
    pty_init();
    size_t pts_dir_idx = 0;
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        if (g_nodes[i].kind == DEVFS_PTS_DIR) { pts_dir_idx = i; break; }
    }
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        memset(&g_vnodes[i], 0, sizeof(g_vnodes[i]));
        g_vnodes[i].ino = i + 1;
    g_vnodes[i].type = (g_nodes[i].kind == DEVFS_ROOT || g_nodes[i].kind == DEVFS_MISC
                         || g_nodes[i].kind == DEVFS_SHM_DIR || g_nodes[i].kind == DEVFS_PTS_DIR)
                     ? VFS_FT_DIR : VFS_FT_REGULAR;
        g_vnodes[i].mode = (g_vnodes[i].type == VFS_FT_DIR) ? (S_IFDIR | 0555) : (S_IFCHR | 0666);
        vnode_ref_init(&g_vnodes[i], 1);
        g_vnodes[i].parent = (i == 0) ? &g_vnodes[0] : &g_vnodes[0];
        if (g_nodes[i].kind == DEVFS_RTC && strcmp(g_nodes[i].name, "rtc") == 0)
            g_vnodes[i].parent = &g_vnodes[1];
        if (g_nodes[i].kind == DEVFS_PTS)
            g_vnodes[i].parent = &g_vnodes[pts_dir_idx];
        g_vnodes[i].fs_data = &g_nodes[i];
        g_vnodes[i].ops = &g_devfs_ops;
    }
    return &g_vnodes[0];
}

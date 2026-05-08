#include "fs/devfs.h"
#include "fs/file.h"
#include "mm/mm.h"
#include "core/timekeeping.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/random.h"

extern void uart_putc(char c);
extern int  uart_getc(void);

enum {
    DEVFS_ROOT,
    DEVFS_MISC,
    DEVFS_NULL,
    DEVFS_ZERO,
    DEVFS_RANDOM,
    DEVFS_TTY,
    DEVFS_RTC,
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
} devfs_node_t;

static int devfs_lookup(vnode_t *dir, const char *name, vnode_t **out);
static int devfs_stat(vnode_t *vn, kstat_t *st);
static void devfs_release(vnode_t *vn);

static vnode_ops_t g_devfs_ops = {
    .lookup = devfs_lookup,
    .stat = devfs_stat,
    .release = devfs_release,
};

static devfs_node_t g_nodes[] = {
    { DEVFS_ROOT, "" },
    { DEVFS_MISC, "misc" },
    { DEVFS_NULL, "null" },
    { DEVFS_ZERO, "zero" },
    { DEVFS_RANDOM, "random" },
    { DEVFS_RANDOM, "urandom" },
    { DEVFS_TTY,  "tty"  },
    { DEVFS_TTY,  "console" },
    { DEVFS_RTC,  "rtc"  },
    { DEVFS_RTC,  "rtc0" },
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

static int devfs_stdout_write(vfile_t *vf, const char *buf, size_t count) {
    (void)vf;
    for (size_t i = 0; i < count; i++) uart_putc(buf[i]);
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
        { "rtc", DT_CHR }, { "rtc0", DT_CHR },
    };
    static const struct {
        const char *name;
        uint8_t type;
    } misc_entries[] = {
        { ".", DT_DIR }, { "..", DT_DIR }, { "rtc", DT_CHR },
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

static vfile_ops_t g_devfs_tty_ops    = { .read = devfs_stdin_read, .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_dir_ops    = { .read = devfs_null_read,  .readdir = devfs_dir_readdir, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stdin_ops  = { .read = devfs_stdin_read, .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stdout_ops = { .read = devfs_null_read,  .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stderr_ops = { .read = devfs_null_read,  .write = devfs_stdout_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_null_ops   = { .read = devfs_null_read,  .write = devfs_null_write,   .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_zero_ops   = { .read = devfs_zero_read,  .write = devfs_null_write,   .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_random_ops = { .read = devfs_random_read,.write = devfs_null_write,   .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_rtc_ops    = { .read = devfs_null_read,  .write = devfs_null_write,   .ioctl = devfs_ioctl };

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
    } else if (node->kind == DEVFS_MISC && strcmp(name, "rtc") == 0) {
        *out = node_to_vnode(7);
        return 0;
    }
    return -ENOENT;
}

static void fill_char_kstat(kstat_t *st) {
    memset(st, 0, sizeof(*st));
    st->st_mode = S_IFCHR | 0666;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_nlink = 1;
    st->st_blksize = 4096;
}

static int devfs_stat(vnode_t *vn, kstat_t *st) {
    devfs_node_t *node = (devfs_node_t *)vn->fs_data;
    if (!node || !st) return -EINVAL;
    memset(st, 0, sizeof(*st));
    if (node->kind == DEVFS_ROOT || node->kind == DEVFS_MISC) {
        st->st_mode = S_IFDIR | 0555;
        st->st_uid = 0;
        st->st_gid = 0;
        st->st_nlink = 2;
        st->st_blksize = 4096;
    } else {
        fill_char_kstat(st);
    }
    return 0;
}

static void devfs_release(vnode_t *vn) {
    (void)vn;
}

vfile_t *devfs_open_vnode(vnode_t *vn, int flags) {
    devfs_node_t *node = vn ? (devfs_node_t *)vn->fs_data : NULL;
    if (!node)
        return NULL;

    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags;
    refcount_set(&vf->ref_count, 1);
    vf->priv = (void *)(intptr_t)node->kind;

    switch (node->kind) {
    case DEVFS_ROOT:
    case DEVFS_MISC: vf->ops = &g_devfs_dir_ops; break;
    case DEVFS_NULL: vf->ops = &g_devfs_null_ops; break;
    case DEVFS_ZERO: vf->ops = &g_devfs_zero_ops; break;
    case DEVFS_RANDOM: vf->ops = &g_devfs_random_ops; break;
    case DEVFS_TTY:  vf->ops = &g_devfs_tty_ops; break;
    case DEVFS_RTC:  vf->ops = &g_devfs_rtc_ops; break;
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
                  vf->ops == &g_devfs_rtc_ops);
}

int devfs_is_tty_vfile(vfile_t *vf) {
    return vf && (vf->ops == &g_devfs_tty_ops ||
                  vf->ops == &g_devfs_stdin_ops ||
                  vf->ops == &g_devfs_stdout_ops ||
                  vf->ops == &g_devfs_stderr_ops);
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
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        memset(&g_vnodes[i], 0, sizeof(g_vnodes[i]));
        g_vnodes[i].ino = i + 1;
        g_vnodes[i].type = (g_nodes[i].kind == DEVFS_ROOT || g_nodes[i].kind == DEVFS_MISC)
                         ? VFS_FT_DIR : VFS_FT_REGULAR;
        g_vnodes[i].mode = (g_vnodes[i].type == VFS_FT_DIR) ? (S_IFDIR | 0555) : (S_IFCHR | 0666);
        vnode_ref_init(&g_vnodes[i], 1);
        g_vnodes[i].parent = (i == 0) ? &g_vnodes[0] : &g_vnodes[0];
        if (g_nodes[i].kind == DEVFS_RTC && strcmp(g_nodes[i].name, "rtc") == 0)
            g_vnodes[i].parent = &g_vnodes[1];
        g_vnodes[i].fs_data = &g_nodes[i];
        g_vnodes[i].ops = &g_devfs_ops;
    }
    return &g_vnodes[0];
}

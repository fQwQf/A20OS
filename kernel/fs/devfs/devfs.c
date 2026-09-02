#include "fs/devfs.h"
#include "fs/readiness.h"
#include "devfs_internal.h"
#include "fs/file.h"
#include "fs/tty.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/consts.h"
#include "core/defs.h"
#include "core/sync.h"
#include "proc/proc.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"
#include "drivers/char/uart.h"
#include "drivers/audio/audio_core.h"
#include "drivers/gpu/gpu_core.h"
#include "mm/slab.h"

extern void uart_putc(char c);
extern int  uart_getc(void);


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

static int devfs_chmod(vnode_t *vn, int mode) {
    if (!vn) return -ENOENT;
    vn->mode = (vn->mode & ~07777u) | ((uint32_t)mode & 07777u);
    return 0;
}

static int devfs_chown(vnode_t *vn, int uid, int gid) {
    if (!vn) return -ENOENT;
    if (uid >= 0) vn->uid = (uint32_t)uid;
    if (gid >= 0) vn->gid = (uint32_t)gid;
    return 0;
}

static vnode_ops_t g_devfs_ops = {
    .lookup = devfs_lookup,
    .stat = devfs_stat,
    .open = devfs_open_vnode,
    .release = devfs_release,
    .chmod = devfs_chmod,
    .chown = devfs_chown,
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
    STATIC_NODE(DEVFS_FULL, "full", 0x107),
    STATIC_NODE(DEVFS_KMSG, "kmsg", 0x10b),
    STATIC_NODE(DEVFS_TTY, "tty", 0x500),
    STATIC_NODE(DEVFS_TTY, "console", 0x501),
    STATIC_NODE(DEVFS_TTY, "tty0", 0x400),
    STATIC_NODE(DEVFS_TTY, "tty1", 0x401),
    STATIC_NODE(DEVFS_TTY, "tty2", 0x402),
    STATIC_NODE(DEVFS_TTY, "tty3", 0x403),
    STATIC_NODE(DEVFS_RTC, "rtc", 0xfe00),
    STATIC_NODE(DEVFS_RTC, "rtc0", 0xfe00),
    STATIC_NODE(DEVFS_LOOP, "loop0", 0x700),
    STATIC_NODE(DEVFS_LOOP, "loop1", 0x701),
    STATIC_NODE(DEVFS_LOOP, "loop2", 0x702),
    STATIC_NODE(DEVFS_FB, "fb0", 0x1d00),
    STATIC_NODE(DEVFS_INPUT, "event0", 0x1d01),
    /* Linux-compatible evdev location: libseat/seatd only accepts input
     * devices under /dev/input/event*, so expose the same multiplexer there
     * (the root-level alias above stays for existing scripts). */
    STATIC_NODE(DEVFS_INPUT_DIR, "input", 0),
    STATIC_NODE(DEVFS_LOOP, "loop3", 0x703),
    STATIC_NODE(DEVFS_LOOP, "loop4", 0x704),
    STATIC_NODE(DEVFS_LOOP, "loop5", 0x705),
    STATIC_NODE(DEVFS_LOOP, "loop6", 0x706),
    STATIC_NODE(DEVFS_LOOP, "loop7", 0x707),
    STATIC_NODE(DEVFS_LOOP_CTRL, "loop-control", 0x70c),
    STATIC_NODE(DEVFS_PTMX, "ptmx", 0x502),
    STATIC_NODE(DEVFS_BLOCK_DIR, "block", 0),
    STATIC_NODE(DEVFS_CHAR_DIR, "char", 0),
    STATIC_NODE(DEVFS_PTS_DIR, "pts", 0),
    STATIC_NODE(DEVFS_DRI_DIR, "dri", 0),
    STATIC_NODE(DEVFS_DRM, "card0", 0xe200),
    STATIC_NODE(DEVFS_SND_DIR, "snd", 0),
    STATIC_NODE(DEVFS_ALSA_CTL, "controlC0", 0x1400),
    STATIC_NODE(DEVFS_ALSA_PCM, "pcmC0D0p", 0x1401),
    STATIC_NODE(DEVFS_ALSA_PCM, "pcmC0D0c", 0x1402),
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
        { "block", DT_DIR },
        { "char", DT_DIR },
        { "dri", DT_DIR },
        { "snd", DT_DIR },
        { "shm", DT_DIR },
        { "input", DT_DIR },
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
    static const struct {
        const char *name;
        uint8_t type;
    } dri_entries[] = {
        { ".", DT_DIR }, { "..", DT_DIR }, { "card0", DT_CHR },
    };
    static const struct {
        const char *name;
        uint8_t type;
    } input_entries[] = {
        { ".", DT_DIR }, { "..", DT_DIR }, { "event0", DT_CHR },
    };
    static const struct {
        const char *name;
        uint8_t type;
    } snd_entries[] = {
        { ".", DT_DIR }, { "..", DT_DIR },
        { "controlC0", DT_CHR }, { "pcmC0D0p", DT_CHR }, { "pcmC0D0c", DT_CHR },
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
    } else if (kind == DEVFS_DRI_DIR) {
        entries_void = dri_entries;
        nentries = sizeof(dri_entries) / sizeof(dri_entries[0]);
    } else if (kind == DEVFS_INPUT_DIR) {
        entries_void = input_entries;
        nentries = sizeof(input_entries) / sizeof(input_entries[0]);
    } else if (kind == DEVFS_SND_DIR) {
        entries_void = snd_entries;
        nentries = sizeof(snd_entries) / sizeof(snd_entries[0]);
    } else if (kind == DEVFS_BLOCK_DIR || kind == DEVFS_CHAR_DIR) {
        entries_void = NULL;
        nentries = 0;
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
    } else if (kind == DEVFS_INPUT_DIR && idx >= nentries) {
        /* Append every published input class device as /dev/input/eventN,
         * starting at index 1 (event0 is the static alias above).  Keep the
         * listing consistent with devfs_lookup so /dev/input/eventN can be
         * opened for every node the udev database knows about. */
        unsigned visible = (unsigned)(idx - nentries);
        unsigned ordinal = 1;
        for (;;) {
            class_device_t *cdev = class_device_get_by_type(DEV_CLASS_INPUT,
                                                             ordinal);
            if (!cdev) {
                /* also try sparse indices beyond the dense run */
                class_device_t *next = class_device_get_nth(ordinal + 1);
                if (!next)
                    break;
                class_device_put(next);
                ordinal++;
                continue;
            }
            if (visible > 0) {
                visible--;
                class_device_put(cdev);
                ordinal++;
                continue;
            }
            char name[32];
            snprintf(name, sizeof(name), "event%u", ordinal);
            size_t namelen = strlen(name);
            size_t reclen = (offsetof(vfs_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
            if (total + reclen > count) {
                class_device_put(cdev);
                break;
            }
            vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
            d->d_ino = 0x20000U + ((uint64_t)DEV_CLASS_INPUT << 8) +
                       (ordinal & 0xffU);
            d->d_off = (int64_t)(idx + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = DT_CHR;
            memcpy(d->d_name, name, namelen + 1);
            total += reclen;
            idx++;
            class_device_put(cdev);
            ordinal++;
        }
    } else if ((kind == DEVFS_BLOCK_DIR || kind == DEVFS_CHAR_DIR) && idx >= nentries) {
        /* List all class devices as major:minor, matching Linux /dev/block
         * and /dev/char conventions so eudev can resolve them. */
        unsigned want_type = (kind == DEVFS_BLOCK_DIR) ? DEV_CLASS_BLOCK : DEV_CLASS_CHAR;
        unsigned visible = (unsigned)(idx - nentries);
        unsigned ordinal = 0;
        for (;;) {
            class_device_t *cdev = class_device_get_nth(ordinal++);
            if (!cdev) break;
            if (cdev->class_type != want_type) {
                class_device_put(cdev);
                continue;
            }
            if (visible > 0) {
                visible--;
                class_device_put(cdev);
                continue;
            }
            unsigned major = (unsigned)(cdev->devt >> 8);
            unsigned minor = (unsigned)(cdev->devt & 0xffU);
            char name[16];
            snprintf(name, sizeof(name), "%u:%u", major, minor);
            size_t namelen = strlen(name);
            size_t reclen = (offsetof(vfs_dirent64_t, d_name) + namelen + 1 + 7) & ~7UL;
            if (total + reclen > count) {
                class_device_put(cdev);
                break;
            }
            vfs_dirent64_t *d = (vfs_dirent64_t *)(out + total);
            d->d_ino = 0x30000U + cdev->index;
            d->d_off = (int64_t)(idx + 1);
            d->d_reclen = (uint16_t)reclen;
            d->d_type = (kind == DEVFS_BLOCK_DIR) ? DT_BLK : DT_CHR;
            memcpy(d->d_name, name, namelen + 1);
            total += reclen;
            idx++;
            class_device_put(cdev);
        }
    }
    vf->offset = idx;
    return (int)total;
}

/* Linux console (VT/KD) ioctl numbers — asm-generic, same on all archs. */


static int devfs_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    int kind = (int)(intptr_t)vf->priv;
    if (kind == DEVFS_TTY)
        return tty_console_ioctl(req, arg);
    if (kind == DEVFS_RTC)
        return devfs_rtc_ioctl(req, arg);
    return -ENOTTY;
}

extern int  loop_dev_read(int idx, char *buf, size_t count, size_t offset);
extern int  loop_dev_write(int idx, const char *buf, size_t count, size_t offset);
extern int  loop_dev_ioctl(vfile_t *vf, unsigned long req, void *arg);
extern int  loop_control_ioctl(unsigned long req, void *arg);
extern int  pty_alloc_and_open(void);
extern int  pty_master_read(int idx, char *buf, size_t count, int nonblock);
extern int  pty_master_write(int idx, const char *buf, size_t count);
extern int  pty_master_ioctl(int idx, unsigned long req, void *arg);
extern int  pty_master_poll(int idx, short events);
extern void pty_master_close(int idx);
extern int  pty_slave_read(int idx, char *buf, size_t count, int nonblock);
extern int  pty_slave_write(int idx, const char *buf, size_t count);
extern int  pty_slave_ioctl(int idx, unsigned long req, void *arg);
extern int  pty_slave_poll(int idx, short events);
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

long devfs_noop_lseek(vfile_t *vf, long offset, int whence) {
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
    return pty_master_read(idx, buf, count, (vf->flags & O_NONBLOCK) != 0);
}
static int devfs_ptm_write(vfile_t *vf, const char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_master_write(idx, buf, count);
}
static int devfs_ptm_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_master_ioctl(idx, req, arg);
}
static int devfs_ptm_poll(vfile_t *vf, short events) {
    return pty_master_poll((int)(intptr_t)vf->priv, events);
}
static int devfs_ptm_close(vfile_t *vf) {
    int idx = (int)(intptr_t)vf->priv;
    pty_master_close(idx);
    return 0;
}
static vfile_ops_t g_devfs_ptm_ops = { .read = devfs_ptm_read, .write = devfs_ptm_write, .ioctl = devfs_ptm_ioctl, .poll = devfs_ptm_poll, .close = devfs_ptm_close };

static int devfs_pts_read(vfile_t *vf, char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_slave_read(idx, buf, count, (vf->flags & O_NONBLOCK) != 0);
}
static int devfs_pts_write(vfile_t *vf, const char *buf, size_t count) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_slave_write(idx, buf, count);
}
static int devfs_pts_ioctl(vfile_t *vf, unsigned long req, void *arg) {
    int idx = (int)(intptr_t)vf->priv;
    return pty_slave_ioctl(idx, req, arg);
}
static int devfs_pts_poll(vfile_t *vf, short events) {
    return pty_slave_poll((int)(intptr_t)vf->priv, events);
}
static int devfs_pts_close(vfile_t *vf) {
    int idx = (int)(intptr_t)vf->priv;
    pty_slave_close(idx);
    return 0;
}
static vfile_ops_t g_devfs_pts_ops = { .read = devfs_pts_read, .write = devfs_pts_write, .ioctl = devfs_pts_ioctl, .poll = devfs_pts_poll, .close = devfs_pts_close };

static size_t devfs_tty_poll_sources(vfile_t *vf, short events,
                                     readiness_source_t *sources, size_t max)
{
    (void)vf;
    if (!sources || max == 0 || !(events & POLLIN))
        return 0;
    sources[0] = (readiness_source_t){ uart_read_wait_queue(), 0, 0 };
    return 1;
}

static vfile_ops_t g_devfs_tty_ops    = { .read = tty_console_read, .write = tty_console_write, .ioctl = devfs_ioctl, .poll_sources = devfs_tty_poll_sources };
static vfile_ops_t g_devfs_dir_ops    = { .read = devfs_null_read,  .readdir = devfs_dir_readdir, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stdin_ops  = { .read = tty_console_read, .write = tty_console_write, .ioctl = devfs_ioctl, .poll_sources = devfs_tty_poll_sources };
static vfile_ops_t g_devfs_stdout_ops = { .read = devfs_null_read,  .write = tty_console_write, .ioctl = devfs_ioctl };
static vfile_ops_t g_devfs_stderr_ops = { .read = devfs_null_read,  .write = tty_console_write, .ioctl = devfs_ioctl };

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
        ret = audio_device_ioctl(dev, ops, req, arg);
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
    class_device_t *cdev = vf ? (class_device_t *)vf->priv : NULL;
    int ret = 0;
    if (cdev && cdev->class_type == DEV_CLASS_AUDIO &&
        class_device_call_begin(cdev) == 0) {
        device_t *dev = cdev->dev;
        const audio_dev_ops_t *ops = dev->drv->class_ops;
        ret = audio_device_close(dev, ops);
        class_device_call_end(cdev);
    }
    if (cdev)
        class_device_put(cdev);
    return ret;
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
    } else if (node->kind == DEVFS_DRI_DIR) {
        for (size_t i = 1; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
            if (g_nodes[i].kind == DEVFS_DRM && strcmp(name, g_nodes[i].name) == 0) {
                *out = node_to_vnode(i);
                return 0;
            }
        }
    } else if (node->kind == DEVFS_INPUT_DIR) {
        /* Static alias first (event0 from g_nodes), then dynamic evdev
         * nodes reflecting every published input class device, so the
         * Linux-style /dev/input/eventN namespace stays in sync with
         * /sys/class/input/eventN and the udev database. */
        for (size_t i = 1; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
            if (g_nodes[i].kind == DEVFS_INPUT && strcmp(name, g_nodes[i].name) == 0) {
                *out = node_to_vnode(i);
                return 0;
            }
        }
        if (strncmp(name, "event", 5) == 0 && name[5] != '\0') {
            char numbuf[16];
            size_t ni = 0;
            const char *p = name + 5;
            while (*p >= '0' && *p <= '9' && ni < sizeof(numbuf) - 1)
                numbuf[ni++] = *p++;
            numbuf[ni] = '\0';
            if (*p == '\0' && ni > 0) {
                unsigned index = 0;
                for (size_t k = 0; k < ni; k++)
                    index = index * 10 + (unsigned)(numbuf[k] - '0');
                char cname[32];
                snprintf(cname, sizeof(cname), "event%u", index);
                class_device_t *cdev = class_device_get_by_name(cname);
                if (cdev && cdev->class_type == DEV_CLASS_INPUT) {
                    devfs_node_t *dynamic = kcalloc(1, sizeof(*dynamic));
                    vnode_t *vn = kcalloc(1, sizeof(*vn));
                    if (!dynamic || !vn) {
                        kfree(dynamic);
                        kfree(vn);
                        class_device_put(cdev);
                        return -ENOMEM;
                    }
                    dynamic->kind = DEVFS_INPUT;
                    dynamic->name = cname;
                    dynamic->rdev = 0x1d01U + (index & 0xffU);
                    dynamic->class_dev = cdev;
                    dynamic->dynamic = 1;
                    vn->ino = 0x20000U + ((uint64_t)DEV_CLASS_INPUT << 8) +
                              (index & 0xffU);
                    vn->type = VFS_FT_REGULAR;
                    vn->mode = S_IFCHR | 0660;
                    vnode_ref_init(vn, 1);
                    vn->parent = dir;
                    vnode_get(dir);
                    vn->fs_data = dynamic;
                    vn->ops = &g_devfs_ops;
                    *out = vn;
                    return 0;
                }
                class_device_put(cdev);
            }
        }
    } else if (node->kind == DEVFS_SND_DIR) {
        for (size_t i = 1; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
            if ((g_nodes[i].kind == DEVFS_ALSA_CTL ||
                 g_nodes[i].kind == DEVFS_ALSA_PCM) &&
                strcmp(name, g_nodes[i].name) == 0) {
                *out = node_to_vnode(i);
                return 0;
            }
        }
    } else if (node->kind == DEVFS_BLOCK_DIR || node->kind == DEVFS_CHAR_DIR) {
        unsigned want_type = (node->kind == DEVFS_BLOCK_DIR) ? DEV_CLASS_BLOCK : DEV_CLASS_CHAR;
        unsigned major = 0, minor = 0;
        const char *p = name;
        while (*p >= '0' && *p <= '9') major = major * 10 + (unsigned)(*p++ - '0');
        if (*p == ':' && p[1] >= '0' && p[1] <= '9') {
            p++;
            while (*p >= '0' && *p <= '9') minor = minor * 10 + (unsigned)(*p++ - '0');
            if (*p == '\0') {
                uint64_t want_devt = ((uint64_t)major << 8) | (minor & 0xffU);
                unsigned ordinal = 0;
                for (;;) {
                    class_device_t *cdev = class_device_get_nth(ordinal++);
                    if (!cdev) break;
                    if (cdev->class_type == want_type && cdev->devt == want_devt) {
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
                        vn->ino = 0x30000U + cdev->index;
                        vn->type = VFS_FT_REGULAR;
                        vn->mode = (want_type == DEV_CLASS_BLOCK ? S_IFBLK : S_IFCHR) | 0660;
                        vnode_ref_init(vn, 1);
                        vn->parent = dir;
                        vnode_get(dir);
                        vn->fs_data = dynamic;
                        vn->ops = &g_devfs_ops;
                        *out = vn;
                        return 0;
                    }
                    class_device_put(cdev);
                }
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
        node->kind == DEVFS_SHM_DIR || node->kind == DEVFS_PTS_DIR ||
        node->kind == DEVFS_DRI_DIR || node->kind == DEVFS_SND_DIR ||
        node->kind == DEVFS_INPUT_DIR || node->kind == DEVFS_BLOCK_DIR ||
        node->kind == DEVFS_CHAR_DIR) {
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
    case DEVFS_FULL: vf->ops = &g_devfs_full_ops; break;
    case DEVFS_KMSG: vf->ops = &g_devfs_kmsg_ops; break;
    case DEVFS_TTY:  vf->ops = &g_devfs_tty_ops; break;
    case DEVFS_RTC:  vf->ops = &g_devfs_rtc_ops; break;
    case DEVFS_FB: {
        task_t *_cur = proc_current();
        printf("[DEVFS] open fb0: pid=%d name=%s gpu_dev=%p\n",
               _cur ? _cur->pid : -1,
               _cur ? _cur->name : "?",
               gpu_device_get_default());
        vf->ops = &g_devfs_fb_ops;
        break;
    }
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
    case DEVFS_DRI_DIR:
        vf->ops = &g_devfs_dir_ops;
        break;
    case DEVFS_INPUT_DIR:
        vf->ops = &g_devfs_dir_ops;
        break;
    case DEVFS_SND_DIR:
        vf->ops = &g_devfs_dir_ops;
        break;
    case DEVFS_ALSA_CTL: {
        extern struct vfile *alsa_control_create_vfile(void);
        vfile_t *avf = alsa_control_create_vfile();
        if (!avf) {
            vnode_put(vf->vnode);
            vfile_free(vf);
            return NULL;
        }
        vnode_put(vf->vnode);
        vfile_free(vf);
        avf->vnode = vn;
        vnode_get(vn);
        return avf;
    }
    case DEVFS_ALSA_PCM: {
        int playback = strcmp(node->name, "pcmC0D0c") != 0;
        extern struct vfile *alsa_pcm_create_vfile(int playback);
        vfile_t *avf = alsa_pcm_create_vfile(playback);
        if (!avf) {
            vnode_put(vf->vnode);
            vfile_free(vf);
            return NULL;
        }
        vnode_put(vf->vnode);
        vfile_free(vf);
        avf->vnode = vn;
        vnode_get(vn);
        return avf;
    }
    case DEVFS_DRM: {
        /* Each open of /dev/dri/card0 creates a fresh DRM context. */
        extern struct vfile *drm_create_vfile(void);
        vfile_t *drm = drm_create_vfile();
        if (!drm) {
            vnode_put(vf->vnode);
            vfile_free(vf);
            return NULL;
        }
        /* Adopt the devfs vnode so stat/fstat on the fd works. */
        vnode_put(vf->vnode);
        vfile_free(vf);
        drm->vnode = vn;
        vnode_get(vn);
        return drm;
    }
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

int devfs_is_fb_vfile(vfile_t *vf) {
    return vf && vf->ops == &g_devfs_fb_ops;
}


vfile_t *devfs_create_stdio(int fd) {
    if (fd == STDIN_FILENO) return &g_stdin_file;
    if (fd == STDOUT_FILENO) return &g_stdout_file;
    if (fd == STDERR_FILENO) return &g_stderr_file;
    return NULL;
}

vnode_t *devfs_mount(void) {
    tty_console_init();
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
    size_t dri_dir_idx = 0;
    size_t snd_dir_idx = 0;
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        if (g_nodes[i].kind == DEVFS_PTS_DIR) { pts_dir_idx = i; break; }
    }
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        if (g_nodes[i].kind == DEVFS_DRI_DIR) { dri_dir_idx = i; break; }
    }
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        if (g_nodes[i].kind == DEVFS_SND_DIR) { snd_dir_idx = i; break; }
    }
    for (size_t i = 0; i < sizeof(g_nodes) / sizeof(g_nodes[0]); i++) {
        memset(&g_vnodes[i], 0, sizeof(g_vnodes[i]));
        g_vnodes[i].ino = i + 1;
    g_vnodes[i].type = (g_nodes[i].kind == DEVFS_ROOT || g_nodes[i].kind == DEVFS_MISC
                         || g_nodes[i].kind == DEVFS_SHM_DIR || g_nodes[i].kind == DEVFS_PTS_DIR
                         || g_nodes[i].kind == DEVFS_DRI_DIR || g_nodes[i].kind == DEVFS_SND_DIR
                         || g_nodes[i].kind == DEVFS_INPUT_DIR)
                     ? VFS_FT_DIR : VFS_FT_REGULAR;
        g_vnodes[i].mode = (g_vnodes[i].type == VFS_FT_DIR) ? (S_IFDIR | 0555) : (S_IFCHR | 0666);
        vnode_ref_init(&g_vnodes[i], 1);
        g_vnodes[i].parent = (i == 0) ? &g_vnodes[0] : &g_vnodes[0];
        if (g_nodes[i].kind == DEVFS_RTC && strcmp(g_nodes[i].name, "rtc") == 0)
            g_vnodes[i].parent = &g_vnodes[1];
        if (g_nodes[i].kind == DEVFS_PTS)
            g_vnodes[i].parent = &g_vnodes[pts_dir_idx];
        if (g_nodes[i].kind == DEVFS_DRM)
            g_vnodes[i].parent = &g_vnodes[dri_dir_idx];
        if (g_nodes[i].kind == DEVFS_ALSA_CTL || g_nodes[i].kind == DEVFS_ALSA_PCM)
            g_vnodes[i].parent = &g_vnodes[snd_dir_idx];
        g_vnodes[i].fs_data = &g_nodes[i];
        g_vnodes[i].ops = &g_devfs_ops;
    }
    return &g_vnodes[0];
}

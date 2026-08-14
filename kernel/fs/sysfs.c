/*
 * A20OS — sysfs: Minimal /sys filesystem
 *
 * Provides the small Linux-compatible sysfs surface currently consumed by
 * userspace: loop block devices for LTP and DRM display metadata.
 *
 * Structure exposed:
 *   /sys/               (directory)
 *   /sys/block/         (directory)
 *   /sys/block/loop0/   (directory)   .. loop7
 *   /sys/block/loopX/dev   (file)     "7:N\n"
 *   /sys/block/loopX/size  (file)     "0\n" (placeholder)
 *   /sys/block/loopX/uevent (file)    empty
 *   /sys/class/drm/card0/device/modalias
 *   /sys/class/drm/card0-Virtual-1/{enabled,status,modes}
 */

#include "core/defs.h"
#include "core/string.h"
#include "core/stdio.h"
#include "fs/vfs.h"
#include "fs/file.h"
#include "mm/mm.h"
#include "core/errno.h"
#include "drivers/core/driver_class.h"
#include "net/socket_internal.h"
#include "drivers/gpu/gpu_core.h"
#include "drivers/core/driver_core.h"
#include "drivers/core/driver_class.h"

/* ---- sysfs node types ---- */

typedef enum {
    SF_ROOT,            /* /sys */
    SF_BLOCK,           /* /sys/block */
    SF_BLOCK_LOOP,      /* /sys/block/loopN */
    SF_BLOCK_LOOP_DEV,  /* /sys/block/loopN/dev */
    SF_BLOCK_LOOP_SIZE, /* /sys/block/loopN/size */
    SF_BLOCK_LOOP_UEVENT, /* /sys/block/loopN/uevent */
    SF_CLASS,
    SF_DRM,
    SF_DRM_CARD,
    SF_DRM_CONNECTOR,
    SF_DRM_CARD_DEVICE,
    SF_DRM_MODALIAS,
    SF_DRM_ENABLED,
    SF_DRM_STATUS,
    SF_DRM_MODES,
    SF_CLASS_TYPE,
    SF_CLASS_DEVICE,
    SF_CLASS_DEVICE_DEV,
    SF_CLASS_DEVICE_UEVENT, /* /sys/class/<sub>/<name>/uevent */
    SF_CLASS_DEVICE_SUBSYS, /* /sys/class/<sub>/<name>/subsystem (symlink) */
    SF_DEVICES,          /* /sys/devices */
    SF_DEVICES_VIRTUAL,  /* /sys/devices/virtual */
    SF_DEVICES_VSUB,     /* /sys/devices/virtual/<subsystem> */
    SF_DEVICES_VDEV,     /* /sys/devices/virtual/<subsystem>/<name> */
    SF_DEVICES_VDEV_UEVENT, /* .../<name>/uevent */
    SF_DEVICES_VDEV_DEV,    /* .../<name>/dev */
    SF_DEV,             /* /sys/dev */
    SF_DEV_CHAR,        /* /sys/dev/char */
    SF_DEV_CHAR_ENTRY,  /* /sys/dev/char/<maj>:<min> */
    SF_DEV_CHAR_UEVENT, /* /sys/dev/char/<maj>:<min>/uevent */
    SF_DEV_CHAR_DEVICE, /* /sys/dev/char/<maj>:<min>/device */
    SF_DEV_CHAR_DEVICE_DRM, /* /sys/dev/char/<maj>:<min>/device/drm */
} sf_type_t;

/* Metadata stored in vnode->fs_data */
typedef struct {
    sf_type_t type;
    int       loop_idx;   /* 0-7 for loop nodes, -1 otherwise */
    uint32_t  width;
    uint32_t  height;
    uint32_t  class_type;
    uint64_t  devt;
    class_device_t *class_dev;
} sysfs_meta_t;

/* Full state for open file handles */
typedef struct {
    sf_type_t type;
    int       loop_idx;
    uint32_t  width;
    uint32_t  height;
    uint64_t  devt;
    char      content[128];
    size_t    content_len;
} sysfs_priv_t;

#define MAX_LOOP_DEVS 8
#define LOOP_MAJOR    7

/* ---- helpers ---- */

#define DRM_MAJOR 226

/* Find the class device whose devt matches devt; returns its subsystem and a
 * DEVNAME (path under /dev) derived from the class.  DRM card0 (226:0) is
 * special-cased because it is a static devfs node, not a class device. */
static int sysfs_devchar_name(uint64_t devt, const char **devname_out,
                              const char **subsystem_out)
{
    unsigned maj = (unsigned)((devt >> 8) & 0xffU);
    unsigned min = (unsigned)(devt & 0xffU);
    static char devname[64];
    const char *subsystem = NULL;

    if (maj == DRM_MAJOR && min == 0) {
        subsystem = "drm";
        snprintf(devname, sizeof(devname), "dri/card0");
        *devname_out = devname;
        *subsystem_out = subsystem;
        return 0;
    }

    for (uint32_t type = 1; type <= DEV_CLASS_AUDIO; type++) {
        const char *sub = class_device_subsystem(type);
        if (!sub)
            continue;
        for (unsigned index = 0; index < 256; index++) {
            class_device_t *cdev = class_device_get_by_type(type, index);
            if (!cdev)
                break;
            uint64_t cdevt = cdev->devt;
            class_device_put(cdev);
            if (cdevt != devt)
                continue;
            subsystem = sub;
            if (type == DEV_CLASS_INPUT)
                snprintf(devname, sizeof(devname), "input/%s", cdev->name);
            else if (type == DEV_CLASS_DISPLAY)
                snprintf(devname, sizeof(devname), "%s", cdev->name);
            else if (type == DEV_CLASS_AUDIO)
                snprintf(devname, sizeof(devname), "snd/%s", cdev->name);
            else
                snprintf(devname, sizeof(devname), "%s", cdev->name);
            *devname_out = devname;
            *subsystem_out = subsystem;
            return 0;
        }
    }
    return -ENOENT;
}

/* Absolute index (after "." and "..") into /sys/dev/char: 0 is the DRM card,
 * then every published class device in (type, index) order. */
static int sysfs_devchar_entry_at(unsigned idx, char *name, size_t name_sz)
{
    if (idx == 0) {
        snprintf(name, name_sz, "226:0");
        return 0;
    }
    unsigned remaining = idx - 1;
    for (uint32_t type = 1; type <= DEV_CLASS_AUDIO; type++) {
        const char *sub = class_device_subsystem(type);
        if (!sub)
            continue;
        unsigned count = 0;
        for (unsigned i = 0; i < 256; i++) {
            class_device_t *c = class_device_get_by_type(type, i);
            if (!c)
                break;
            count++;
            class_device_put(c);
        }
        if (remaining < count) {
            class_device_t *c = class_device_get_by_type(type, remaining);
            if (!c)
                return -1;
            uint64_t devt = c->devt;
            class_device_put(c);
            snprintf(name, name_sz, "%lu:%lu",
                     (unsigned long)((devt >> 8) & 0xffU),
                     (unsigned long)(devt & 0xffU));
            return 0;
        }
        remaining -= count;
    }
    return -1;
}

static void sysfs_display_size(uint32_t *width, uint32_t *height)
{
    *width = 1024;
    *height = 768;
    device_t *dev = gpu_device_get_default();
    if (!dev || !dev->drv || !dev->drv->class_ops)
        return;
    const gpu_dev_ops_t *ops = (const gpu_dev_ops_t *)dev->drv->class_ops;
    uint32_t bpp;
    (void)ops->get_info(dev, width, height, &bpp);
}

static sysfs_meta_t *sysfs_meta_create(sf_type_t type, int loop_idx)
{
    sysfs_meta_t *m = (sysfs_meta_t *)kmalloc(sizeof(*m));
    if (!m) return NULL;
    memset(m, 0, sizeof(*m));
    m->type = type;
    m->loop_idx = loop_idx;
    if (type == SF_DRM || type == SF_DRM_CARD || type == SF_DRM_CONNECTOR) {
        sysfs_display_size(&m->width, &m->height);
    }
    return m;
}

static sysfs_priv_t *sysfs_priv_create(sf_type_t type, int loop_idx,
                                       uint32_t width, uint32_t height,
                                       uint64_t devt)
{
    sysfs_priv_t *p = (sysfs_priv_t *)kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->type = type;
    p->loop_idx = loop_idx;
    p->width = width;
    p->height = height;
    p->devt = devt;

    /* Generate content on open */
    if (type == SF_BLOCK_LOOP_DEV) {
        int n = snprintf(p->content, sizeof(p->content),
                         "%d:%d\n", LOOP_MAJOR, loop_idx);
        p->content_len = (size_t)(n > 0 ? n : 0);
    } else if (type == SF_BLOCK_LOOP_SIZE) {
        p->content[0] = '0';
        p->content[1] = '\n';
        p->content_len = 2;
    } else if (type == SF_DRM_ENABLED) {
        memcpy(p->content, "enabled\n", 8);
        p->content_len = 8;
    } else if (type == SF_DRM_STATUS) {
        memcpy(p->content, "connected\n", 10);
        p->content_len = 10;
    } else if (type == SF_DRM_MODES) {
        int n = snprintf(p->content, sizeof(p->content), "%ux%u\n", width, height);
        p->content_len = (size_t)(n > 0 ? n : 0);
    } else if (type == SF_DRM_MODALIAS) {
        const char modalias[] = "of:NgpuTdisplayCvirtio,gpuC\n";
        memcpy(p->content, modalias, sizeof(modalias) - 1);
        p->content_len = sizeof(modalias) - 1;
    } else if (type == SF_CLASS_DEVICE_DEV) {
        int n = snprintf(p->content, sizeof(p->content), "%lu:%lu\n",
                         (unsigned long)(devt >> 8),
                         (unsigned long)(devt & 0xffU));
        p->content_len = (size_t)(n > 0 ? n : 0);
    } else if (type == SF_CLASS_DEVICE_UEVENT) {
        const char *devname = NULL, *subsystem = NULL;
        if (sysfs_devchar_name(devt, &devname, &subsystem) == 0) {
            const char *base = strrchr(devname, '/');
            base = base ? base + 1 : devname;
            int n = snprintf(p->content, sizeof(p->content),
                             "MAJOR=%lu\nMINOR=%lu\nDEVNAME=%s\nSUBSYSTEM=%s\n"
                             "DEVPATH=/class/%s/%s\n",
                             (unsigned long)((devt >> 8) & 0xffU),
                             (unsigned long)(devt & 0xffU),
                             devname, subsystem, subsystem, base);
            p->content_len = (size_t)(n > 0 ? n : 0);
        } else {
            p->content_len = 0;
        }
    } else if (type == SF_DEV_CHAR_UEVENT) {
        const char *devname = NULL, *subsystem = NULL;
        if (sysfs_devchar_name(devt, &devname, &subsystem) == 0) {
            int n = snprintf(p->content, sizeof(p->content),
                             "MAJOR=%lu\nMINOR=%lu\nDEVNAME=%s\nSUBSYSTEM=%s\n",
                             (unsigned long)((devt >> 8) & 0xffU),
                             (unsigned long)(devt & 0xffU),
                             devname, subsystem);
            p->content_len = (size_t)(n > 0 ? n : 0);
        } else {
            p->content_len = 0;
        }
    } else if (type == SF_DEVICES_VDEV_DEV) {
        int n = snprintf(p->content, sizeof(p->content), "%lu:%lu\n",
                         (unsigned long)((devt >> 8) & 0xffU),
                         (unsigned long)(devt & 0xffU));
        p->content_len = (size_t)(n > 0 ? n : 0);
    } else if (type == SF_DEVICES_VDEV_UEVENT) {
        const char *devname = NULL, *subsystem = NULL;
        if (sysfs_devchar_name(devt, &devname, &subsystem) == 0) {
            int n = snprintf(p->content, sizeof(p->content),
                             "MAJOR=%lu\nMINOR=%lu\nDEVNAME=%s\nSUBSYSTEM=%s\n"
                             "DEVPATH=/devices/virtual/%s/%s\n",
                             (unsigned long)((devt >> 8) & 0xffU),
                             (unsigned long)(devt & 0xffU),
                             devname, subsystem, subsystem,
                             strrchr(devname, '/') ? strrchr(devname, '/') + 1
                                                   : devname);
            p->content_len = (size_t)(n > 0 ? n : 0);
        } else {
            p->content_len = 0;
        }
    } else {
        p->content_len = 0;
    }

    return p;
}

/* ---- vnode operations ---- */

/* Map a subsystem name ("input", "drm", ...) to its device class, or
 * DEV_CLASS_NONE if unknown. */
static uint32_t sysfs_class_type_by_subsystem(const char *name)
{
    for (uint32_t type = 1; type <= DEV_CLASS_AUDIO; type++) {
        const char *sub = class_device_subsystem(type);
        if (sub && strcmp(sub, name) == 0)
            return type;
    }
    return DEV_CLASS_NONE;
}

static int sysfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    if (!name || !*name) return -ENOENT;

    sysfs_meta_t *dm = (sysfs_meta_t *)dir->fs_data;
    if (!dm) return -ENOENT;

    sf_type_t child_type = SF_ROOT;
    int child_idx = -1;

    class_device_t *dynamic_cdev = NULL;
    uint32_t dynamic_class = DEV_CLASS_NONE;
    if (dm->type == SF_ROOT && strcmp(name, "block") == 0) {
        child_type = SF_BLOCK;
    } else if (dm->type == SF_ROOT && strcmp(name, "class") == 0) {
        child_type = SF_CLASS;
    } else if (dm->type == SF_ROOT && strcmp(name, "dev") == 0) {
        child_type = SF_DEV;
    } else if (dm->type == SF_ROOT && strcmp(name, "devices") == 0) {
        child_type = SF_DEVICES;
    } else if (dm->type == SF_DEVICES && strcmp(name, "virtual") == 0) {
        child_type = SF_DEVICES_VIRTUAL;
    } else if (dm->type == SF_DEVICES_VIRTUAL) {
        uint32_t ct = sysfs_class_type_by_subsystem(name);
        if (ct == DEV_CLASS_NONE)
            return -ENOENT;
        child_type = SF_DEVICES_VSUB;
        dynamic_class = ct;
    } else if (dm->type == SF_DEVICES_VSUB) {
        class_device_t *cdev = class_device_get_by_name(name);
        if (!cdev || cdev->class_type != dm->class_type) {
            class_device_put(cdev);
            return -ENOENT;
        }
        child_type = SF_DEVICES_VDEV;
        dynamic_class = dm->class_type;
        dynamic_cdev = cdev;
    } else if (dm->type == SF_DEVICES_VDEV && strcmp(name, "uevent") == 0) {
        child_type = SF_DEVICES_VDEV_UEVENT;
        dynamic_class = dm->class_type;
        dynamic_cdev = dm->class_dev;
        class_device_get(dynamic_cdev);
    } else if (dm->type == SF_DEVICES_VDEV && strcmp(name, "dev") == 0) {
        child_type = SF_DEVICES_VDEV_DEV;
        dynamic_class = dm->class_type;
        dynamic_cdev = dm->class_dev;
        class_device_get(dynamic_cdev);
    } else if (dm->type == SF_DEV && strcmp(name, "char") == 0) {
        child_type = SF_DEV_CHAR;
    } else if (dm->type == SF_DEV_CHAR) {
        /* /sys/dev/char/<maj>:<min>: accept only existing devices */
        const char *p = name;
        unsigned maj = 0, min = 0;
        while (*p >= '0' && *p <= '9') {
            maj = maj * 10 + (unsigned)(*p - '0');
            p++;
        }
        if (*p != ':')
            return -ENOENT;
        p++;
        while (*p >= '0' && *p <= '9') {
            min = min * 10 + (unsigned)(*p - '0');
            p++;
        }
        if (*p != '\0')
            return -ENOENT;
        uint64_t devt = ((uint64_t)(maj & 0xffU) << 8) | (min & 0xffU);
        const char *devname = NULL, *subsystem = NULL;
        if (sysfs_devchar_name(devt, &devname, &subsystem) < 0)
            return -ENOENT;
        child_type = SF_DEV_CHAR_ENTRY;
        child_idx = (int)(devt & 0xffffU);
        dynamic_class = DEV_CLASS_NONE;
    } else if (dm->type == SF_DEV_CHAR_ENTRY && strcmp(name, "uevent") == 0) {
        child_type = SF_DEV_CHAR_UEVENT;
        child_idx = dm->loop_idx;
    } else if (dm->type == SF_DEV_CHAR_ENTRY && strcmp(name, "device") == 0) {
        child_type = SF_DEV_CHAR_DEVICE;
        child_idx = dm->loop_idx;
    } else if (dm->type == SF_DEV_CHAR_DEVICE && strcmp(name, "drm") == 0) {
        child_type = SF_DEV_CHAR_DEVICE_DRM;
        child_idx = dm->loop_idx;
    } else if (dm->type == SF_CLASS && strcmp(name, "drm") == 0) {
        child_type = SF_DRM;
    } else if (dm->type == SF_CLASS) {
        static const struct { const char *name; uint32_t type; } classes[] = {
            { "char", DEV_CLASS_CHAR }, { "block", DEV_CLASS_BLOCK },
            { "net", DEV_CLASS_NET }, { "input", DEV_CLASS_INPUT },
            { "display", DEV_CLASS_DISPLAY }, { "audio", DEV_CLASS_AUDIO },
        };
        for (size_t i = 0; i < ARRAY_SIZE(classes); i++) {
            if (strcmp(name, classes[i].name) == 0) {
                child_type = SF_CLASS_TYPE;
                dynamic_class = classes[i].type;
                break;
            }
        }
        if (dynamic_class == DEV_CLASS_NONE)
            return -ENOENT;
    } else if (dm->type == SF_CLASS_TYPE) {
        dynamic_cdev = class_device_get_by_name(name);
        if (!dynamic_cdev || dynamic_cdev->class_type != dm->class_type) {
            class_device_put(dynamic_cdev);
            return -ENOENT;
        }
        child_type = SF_CLASS_DEVICE;
        dynamic_class = dm->class_type;
    } else if (dm->type == SF_CLASS_DEVICE && strcmp(name, "dev") == 0) {
        child_type = SF_CLASS_DEVICE_DEV;
        dynamic_class = dm->class_type;
        dynamic_cdev = dm->class_dev;
        class_device_get(dynamic_cdev);
    } else if (dm->type == SF_CLASS_DEVICE && strcmp(name, "uevent") == 0) {
        child_type = SF_CLASS_DEVICE_UEVENT;
        dynamic_class = dm->class_type;
        dynamic_cdev = dm->class_dev;
        class_device_get(dynamic_cdev);
    } else if (dm->type == SF_CLASS_DEVICE && strcmp(name, "subsystem") == 0) {
        child_type = SF_CLASS_DEVICE_SUBSYS;
        dynamic_class = dm->class_type;
        dynamic_cdev = dm->class_dev;
        class_device_get(dynamic_cdev);
    } else if (dm->type == SF_DRM && strcmp(name, "card0") == 0) {
        child_type = SF_DRM_CARD;
    } else if (dm->type == SF_DRM && strcmp(name, "card0-Virtual-1") == 0) {
        child_type = SF_DRM_CONNECTOR;
    } else if (dm->type == SF_DRM_CARD) {
        if (strcmp(name, "device") == 0) child_type = SF_DRM_CARD_DEVICE;
        else return -ENOENT;
    } else if (dm->type == SF_DRM_CARD_DEVICE && strcmp(name, "modalias") == 0) {
        child_type = SF_DRM_MODALIAS;
    } else if (dm->type == SF_DRM_CONNECTOR) {
        if (strcmp(name, "enabled") == 0) child_type = SF_DRM_ENABLED;
        else if (strcmp(name, "status") == 0) child_type = SF_DRM_STATUS;
        else if (strcmp(name, "modes") == 0) child_type = SF_DRM_MODES;
        else return -ENOENT;
    } else if (dm->type == SF_BLOCK) {
        if (name[0] == 'l' && name[1] == 'o' && name[2] == 'o' &&
            name[3] == 'p') {
            const char *num = name + 4;
            int idx = 0;
            if (!*num) return -ENOENT;
            while (*num) {
                if (*num < '0' || *num > '9') return -ENOENT;
                idx = idx * 10 + (*num - '0');
                num++;
            }
            if (idx < 0 || idx >= MAX_LOOP_DEVS) return -ENOENT;
            child_type = SF_BLOCK_LOOP;
            child_idx = idx;
        } else {
            return -ENOENT;
        }
    } else if (dm->type == SF_BLOCK_LOOP) {
        if (strcmp(name, "dev") == 0) {
            child_type = SF_BLOCK_LOOP_DEV;
            child_idx = dm->loop_idx;
        } else if (strcmp(name, "size") == 0) {
            child_type = SF_BLOCK_LOOP_SIZE;
            child_idx = dm->loop_idx;
        } else if (strcmp(name, "uevent") == 0) {
            child_type = SF_BLOCK_LOOP_UEVENT;
            child_idx = dm->loop_idx;
        } else {
            return -ENOENT;
        }
    } else {
        return -ENOENT;
    }

    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn) {
        class_device_put(dynamic_cdev);
        return -ENOMEM;
    }
    memset(vn, 0, sizeof(*vn));

    int is_dir = (child_type == SF_ROOT || child_type == SF_BLOCK ||
                  child_type == SF_BLOCK_LOOP || child_type == SF_CLASS ||
                  child_type == SF_DRM || child_type == SF_DRM_CARD ||
                  child_type == SF_DRM_CONNECTOR || child_type == SF_DRM_CARD_DEVICE ||
                  child_type == SF_CLASS_TYPE || child_type == SF_CLASS_DEVICE ||
                  child_type == SF_DEVICES || child_type == SF_DEVICES_VIRTUAL ||
                  child_type == SF_DEVICES_VSUB || child_type == SF_DEVICES_VDEV ||
                  child_type == SF_DEV || child_type == SF_DEV_CHAR ||
                  child_type == SF_DEV_CHAR_ENTRY ||
                  child_type == SF_DEV_CHAR_DEVICE ||
                  child_type == SF_DEV_CHAR_DEVICE_DRM);

    vn->ino = (uint64_t)((child_type << 8) | ((child_idx + 1) & 0xFF));
    if (child_type == SF_CLASS_DEVICE_SUBSYS) {
        vn->type = VFS_FT_SYMLINK;
        vn->mode = S_IFLNK | 0777;
    } else if (child_type == SF_CLASS_DEVICE_UEVENT) {
        /* uevent files are writable so udevadm trigger can coldplug. */
        vn->type = VFS_FT_REGULAR;
        vn->mode = S_IFREG | 0644;
    } else {
        vn->type = is_dir ? VFS_FT_DIR : VFS_FT_REGULAR;
        vn->mode = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    }
    vnode_ref_init(vn, 1);
    vn->parent = dir;
    vnode_get(dir);
    vn->ops = dir->ops;

    sysfs_meta_t *meta = sysfs_meta_create(child_type, child_idx);
    if (meta) {
        meta->class_type = dynamic_class;
        meta->class_dev = dynamic_cdev;
        meta->devt = dynamic_cdev ? dynamic_cdev->devt : 0;
        if (child_type == SF_DEV_CHAR_ENTRY || child_type == SF_DEV_CHAR_UEVENT ||
            child_type == SF_DEV_CHAR_DEVICE ||
            child_type == SF_DEV_CHAR_DEVICE_DRM)
            meta->devt = (uint64_t)((uint32_t)child_idx & 0xffffU);
        if (dm->type == SF_DRM_CONNECTOR) {
            meta->width = dm->width;
            meta->height = dm->height;
        }
        if (child_type == SF_DRM_MODES) {
            char modes[32];
            int n = snprintf(modes, sizeof(modes), "%ux%u\n", meta->width, meta->height);
            vn->size = (size_t)(n > 0 ? n : 0);
        } else if (child_type == SF_DRM_ENABLED) {
            vn->size = sizeof("enabled\n") - 1;
        } else if (child_type == SF_DRM_STATUS) {
            vn->size = sizeof("connected\n") - 1;
        } else if (child_type == SF_DRM_MODALIAS) {
            vn->size = sizeof("of:NgpuTdisplayCvirtio,gpuC\n") - 1;
        } else if (child_type == SF_BLOCK_LOOP_DEV) {
            char tmp[32];
            int n = snprintf(tmp, sizeof(tmp), "%d:%d\n", LOOP_MAJOR, child_idx);
            vn->size = (size_t)(n > 0 ? n : 0);
        } else if (child_type == SF_BLOCK_LOOP_SIZE) {
            vn->size = 2;
        } else if (child_type == SF_CLASS_DEVICE_DEV) {
            vn->size = 16;
        } else if (child_type == SF_CLASS_DEVICE_UEVENT) {
            vn->size = 96;
        } else if (child_type == SF_DEVICES_VDEV_DEV) {
            vn->size = 16;
        } else if (child_type == SF_DEVICES_VDEV_UEVENT) {
            vn->size = 96;
        } else if (child_type == SF_DEV_CHAR_UEVENT) {
            vn->size = 64;
        }
    } else {
        class_device_put(dynamic_cdev);
        vnode_put(dir);
        kfree(vn);
        return -ENOMEM;
    }
    vn->fs_data = meta;

    *out = vn;
    return 0;
}

static int sysfs_stat(vnode_t *vn, kstat_t *st)
{
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_mode = vn->mode;
    st->st_uid = 0;
    st->st_gid = 0;
    st->st_size = vn->size;
    st->st_nlink = 1;
    return 0;
}

static void sysfs_release(vnode_t *vn)
{
    if (vn->fs_data) {
        sysfs_meta_t *meta = vn->fs_data;
        class_device_put(meta->class_dev);
        kfree(meta);
    }
    vnode_put(vn->parent);
    kfree(vn);
}

static vfile_t *sysfs_open_vnode(vnode_t *vn, int flags);

static int sysfs_readlink(vnode_t *vn, char *buf, size_t sz)
{
    if (!vn || !buf || sz == 0)
        return -EINVAL;
    sysfs_meta_t *dm = (sysfs_meta_t *)vn->fs_data;
    if (!dm || dm->type != SF_CLASS_DEVICE_SUBSYS)
        return -EINVAL;
    const char *sub = class_device_subsystem(dm->class_type);
    if (!sub)
        return -ENOENT;
    int n = snprintf(buf, sz, "/sys/class/%s", sub);
    return (n > 0 && (size_t)n < sz) ? n : -ENAMETOOLONG;
}

static vnode_ops_t g_sysfs_vnode_ops = {
    .lookup  = sysfs_lookup,
    .stat    = sysfs_stat,
    .open    = sysfs_open_vnode,
    .release = sysfs_release,
    .readlink = sysfs_readlink,
};

/* ---- file operations ---- */

static int sysfs_fread(vfile_t *vf, char *buf, size_t count)
{
    if (!vf || !vf->priv) return -EBADF;
    sysfs_priv_t *p = (sysfs_priv_t *)vf->priv;

    if (vf->offset >= p->content_len) return 0;
    size_t avail = p->content_len - vf->offset;
    if (count > avail) count = avail;
    memcpy(buf, p->content + vf->offset, count);
    vf->offset += count;
    return (int)count;
}

static long sysfs_flseek(vfile_t *vf, long offset, int whence)
{
    if (!vf || !vf->priv) return -EBADF;
    sysfs_priv_t *p = (sysfs_priv_t *)vf->priv;
    long new_off;
    switch (whence) {
    case 0: new_off = offset; break;
    case 1: new_off = (long)vf->offset + offset; break;
    case 2: new_off = (long)p->content_len + offset; break;
    default: return -EINVAL;
    }
    if (new_off < 0) return -EINVAL;
    vf->offset = (size_t)new_off;
    return new_off;
}

static int sysfs_class_readdir(vfile_t *vf, sysfs_meta_t *dm,
                               void *dirp, size_t count)
{
    size_t pos = vf->offset;
    size_t written = 0;
    char *out = dirp;
    for (;;) {
        const char *name;
        class_device_t *cdev = NULL;
        if (pos == 0) name = ".";
        else if (pos == 1) name = "..";
        else {
            cdev = class_device_get_by_type(dm->class_type,
                                             (unsigned)(pos - 2));
            if (!cdev)
                break;
            name = cdev->name;
        }
        size_t nlen = strlen(name);
        size_t reclen = (offsetof(vfs_dirent64_t, d_name) + nlen + 1 + 7) & ~7UL;
        if (written + reclen > count) {
            class_device_put(cdev);
            break;
        }
        vfs_dirent64_t *de = (vfs_dirent64_t *)(out + written);
        memset(de, 0, reclen);
        de->d_ino = pos + 1;
        de->d_off = (int64_t)(pos + 1);
        de->d_reclen = (uint16_t)reclen;
        de->d_type = 4;
        memcpy(de->d_name, name, nlen + 1);
        class_device_put(cdev);
        written += reclen;
        pos++;
    }
    vf->offset = pos;
    return (int)written;
}

/* /sys/dev/char/<maj>:<min> directory listing (a vnode holds one devt). */
static int sysfs_devchar_readdir(vfile_t *vf, sysfs_meta_t *dm,
                                 void *dirp, size_t count)
{
    (void)dm;
    size_t pos = vf->offset;
    size_t written = 0;
    char *out = dirp;
    for (;;) {
        const char *name;
        uint8_t dtype;
        if (pos == 0) { name = "."; dtype = 4; }
        else if (pos == 1) { name = ".."; dtype = 4; }
        else if (pos == 2) { name = "device"; dtype = 4; }
        else if (pos == 3) { name = "uevent"; dtype = 8; }
        else break;

        size_t nlen = strlen(name);
        size_t reclen = (offsetof(vfs_dirent64_t, d_name) + nlen + 1 + 7) & ~7UL;
        if (written + reclen > count)
            break;
        vfs_dirent64_t *de = (vfs_dirent64_t *)(out + written);
        memset(de, 0, reclen);
        de->d_ino = pos + 1;
        de->d_off = (int64_t)(pos + 1);
        de->d_reclen = (uint16_t)reclen;
        de->d_type = dtype;
        memcpy(de->d_name, name, nlen + 1);
        written += reclen;
        pos++;
    }
    vf->offset = pos;
    return (int)written;
}

/* /sys/dev/char listing: DRM card0 followed by every published class device. */
static int sysfs_devchar_dir_readdir(vfile_t *vf, void *dirp, size_t count)
{
    size_t pos = vf->offset;
    size_t written = 0;
    char *out = dirp;
    for (;;) {
        const char *name;
        char scratch[32];
        if (pos == 0) name = ".";
        else if (pos == 1) name = "..";
        else {
            if (sysfs_devchar_entry_at((unsigned)(pos - 2), scratch,
                                       sizeof(scratch)) < 0)
                break;
            name = scratch;
        }
        size_t nlen = strlen(name);
        size_t reclen = (offsetof(vfs_dirent64_t, d_name) + nlen + 1 + 7) & ~7UL;
        if (written + reclen > count)
            break;
        vfs_dirent64_t *de = (vfs_dirent64_t *)(out + written);
        memset(de, 0, reclen);
        de->d_ino = pos + 1;
        de->d_off = (int64_t)(pos + 1);
        de->d_reclen = (uint16_t)reclen;
        de->d_type = 4;
        memcpy(de->d_name, name, nlen + 1);
        written += reclen;
        pos++;
    }
    vf->offset = pos;
    return (int)written;
}

static int sysfs_freaddir(vfile_t *vf, void *dirp, size_t count)
{
    if (!vf || !vf->vnode) return -EBADF;
    sysfs_meta_t *dm = (sysfs_meta_t *)vf->vnode->fs_data;
    if (!dm) return -ENOENT;
    if (dm->type == SF_CLASS_TYPE)
        return sysfs_class_readdir(vf, dm, dirp, count);
    if (dm->type == SF_DEVICES_VSUB)
        return sysfs_class_readdir(vf, dm, dirp, count);
    if (dm->type == SF_DEV_CHAR)
        return sysfs_devchar_dir_readdir(vf, dirp, count);
    if (dm->type == SF_DEV_CHAR_ENTRY)
        return sysfs_devchar_readdir(vf, dm, dirp, count);

    /* Build list of entries for this directory */
    struct {
        const char *name;
        uint8_t dtype;
    } entries[16];
    int nent = 0;

    entries[nent].name = ".";  entries[nent].dtype = 4; nent++;
    entries[nent].name = ".."; entries[nent].dtype = 4; nent++;

    if (dm->type == SF_ROOT) {
        entries[nent].name = "block"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "class"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "dev"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "devices"; entries[nent].dtype = 4; nent++;
    } else if (dm->type == SF_DEVICES) {
        entries[nent].name = "virtual"; entries[nent].dtype = 4; nent++;
    } else if (dm->type == SF_DEVICES_VIRTUAL) {
        entries[nent].name = "drm"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "char"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "block"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "net"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "input"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "display"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "audio"; entries[nent].dtype = 4; nent++;
    } else if (dm->type == SF_DEVICES_VDEV) {
        entries[nent].name = "uevent"; entries[nent].dtype = 8; nent++;
        entries[nent].name = "dev"; entries[nent].dtype = 8; nent++;
    } else if (dm->type == SF_DEV) {
        entries[nent].name = "char"; entries[nent].dtype = 4; nent++;
    } else if (dm->type == SF_CLASS) {
        entries[nent].name = "drm"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "char"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "block"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "net"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "input"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "display"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "audio"; entries[nent].dtype = 4; nent++;
    } else if (dm->type == SF_CLASS_DEVICE) {
        entries[nent].name = "dev"; entries[nent].dtype = 8; nent++;
        entries[nent].name = "uevent"; entries[nent].dtype = 8; nent++;
        entries[nent].name = "subsystem"; entries[nent].dtype = 10; nent++;
    } else if (dm->type == SF_DRM) {
        entries[nent].name = "card0"; entries[nent].dtype = 4; nent++;
        entries[nent].name = "card0-Virtual-1"; entries[nent].dtype = 4; nent++;
    } else if (dm->type == SF_DRM_CARD) {
        entries[nent].name = "device"; entries[nent].dtype = 4; nent++;
    } else if (dm->type == SF_DRM_CARD_DEVICE) {
        entries[nent].name = "modalias"; entries[nent].dtype = 8; nent++;
    } else if (dm->type == SF_DRM_CONNECTOR) {
        entries[nent].name = "enabled"; entries[nent].dtype = 8; nent++;
        entries[nent].name = "status"; entries[nent].dtype = 8; nent++;
        entries[nent].name = "modes"; entries[nent].dtype = 8; nent++;
    } else if (dm->type == SF_BLOCK) {
        for (int i = 0; i < MAX_LOOP_DEVS && nent < 15; i++) {
            static const char *loop_names[8] = {
                "loop0", "loop1", "loop2", "loop3",
                "loop4", "loop5", "loop6", "loop7"
            };
            entries[nent].name = loop_names[i];
            entries[nent].dtype = 4;
            nent++;
        }
    } else if (dm->type == SF_BLOCK_LOOP) {
        entries[nent].name = "dev";    entries[nent].dtype = 8; nent++;
        entries[nent].name = "size";   entries[nent].dtype = 8; nent++;
        entries[nent].name = "uevent"; entries[nent].dtype = 8; nent++;
    }

    size_t pos = vf->offset;
    size_t written = 0;
    char *out = (char *)dirp;

    while (pos < (size_t)nent && written < count) {
        const char *name = entries[pos].name;
        size_t nlen = strlen(name);
        size_t reclen = offsetof(vfs_dirent64_t, d_name) + nlen + 1;
        reclen = (reclen + 7) & ~7UL;

        if (written + reclen > count) break;

        vfs_dirent64_t *de = (vfs_dirent64_t *)(out + written);
        memset(de, 0, reclen);
        de->d_ino = pos + 1;
        de->d_off = (int64_t)(pos + 1);
        de->d_reclen = (uint16_t)reclen;
        de->d_type = entries[pos].dtype;
        memcpy(de->d_name, name, nlen + 1);

        written += reclen;
        pos++;
    }

    vf->offset = pos;
    return (int)written;
}

static int sysfs_fclose(vfile_t *vf)
{
    if (vf->priv) kfree(vf->priv);
    vf->priv = NULL;
    return 0;
}

/* Coldplug: writing an action ("add") to a uevent file re-broadcasts the
 * device's uevent so udevd processes it (runs rules) — the standard trigger
 * path that udevadm trigger relies on. */
static int sysfs_fwrite(vfile_t *vf, const char *buf, size_t count)
{
    if (!vf || !vf->priv) return -EBADF;
    sysfs_priv_t *p = (sysfs_priv_t *)vf->priv;
    if (p->type != SF_CLASS_DEVICE_UEVENT && p->type != SF_DEVICES_VDEV_UEVENT)
        return -EINVAL;
    char action[16];
    size_t n = count < sizeof(action) - 1 ? count : sizeof(action) - 1;
    memcpy(action, buf, n);
    action[n] = 0;
    char *nl = strchr(action, '\n');
    if (nl) *nl = 0;
    if (action[0] == 0)
        return -EINVAL;
    const char *devname = NULL, *subsystem = NULL;
    if (sysfs_devchar_name(p->devt, &devname, &subsystem) < 0)
        return -ENOENT;
    const char *base = strrchr(devname, '/');
    base = base ? base + 1 : devname;
    netlink_uevent_emit(action, subsystem, base, p->devt);
    return (int)count;
}

static vfile_ops_t g_sysfs_fops = {
    .read    = sysfs_fread,
    .write   = sysfs_fwrite,
    .lseek   = sysfs_flseek,
    .readdir = sysfs_freaddir,
    .close   = sysfs_fclose,
};

/* ---- mount & open ---- */

vnode_t *sysfs_mount(void)
{
    vnode_t *root = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!root) return NULL;
    memset(root, 0, sizeof(*root));
    root->ino = 0;
    root->type = VFS_FT_DIR;
    root->mode = S_IFDIR | 0555;
    vnode_ref_init(root, 1);
    root->ops = &g_sysfs_vnode_ops;

    sysfs_meta_t *meta = sysfs_meta_create(SF_ROOT, -1);
    root->fs_data = meta;
    return root;
}

static vfile_t *sysfs_open_vnode(vnode_t *vn, int flags)
{
    vfile_t *vf = vfile_alloc();
    if (!vf) return NULL;
    vf->vnode = vn;
    vf->flags = flags;
    vnode_get(vn);
    vf->ops = &g_sysfs_fops;
    vfile_ref_init(vf, 1);

    sysfs_meta_t *meta = (sysfs_meta_t *)vn->fs_data;
    sysfs_priv_t *priv = sysfs_priv_create(
        meta ? meta->type : SF_ROOT,
        meta ? meta->loop_idx : -1,
        meta ? meta->width : 0,
        meta ? meta->height : 0,
        meta ? meta->devt : 0);
    if (!priv) { vfile_free(vf); return NULL; }
    vf->priv = priv;
    return vf;
}

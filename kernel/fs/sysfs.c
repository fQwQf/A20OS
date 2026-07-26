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
#include "drivers/gpu/gpu_core.h"

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
} sf_type_t;

/* Metadata stored in vnode->fs_data */
typedef struct {
    sf_type_t type;
    int       loop_idx;   /* 0-7 for loop nodes, -1 otherwise */
    uint32_t  width;
    uint32_t  height;
} sysfs_meta_t;

/* Full state for open file handles */
typedef struct {
    sf_type_t type;
    int       loop_idx;
    uint32_t  width;
    uint32_t  height;
    char      content[128];
    size_t    content_len;
} sysfs_priv_t;

#define MAX_LOOP_DEVS 8
#define LOOP_MAJOR    7

/* ---- helpers ---- */

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
                                       uint32_t width, uint32_t height)
{
    sysfs_priv_t *p = (sysfs_priv_t *)kmalloc(sizeof(*p));
    if (!p) return NULL;
    memset(p, 0, sizeof(*p));
    p->type = type;
    p->loop_idx = loop_idx;
    p->width = width;
    p->height = height;

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
    } else {
        p->content_len = 0;
    }

    return p;
}

/* ---- vnode operations ---- */

static int sysfs_lookup(vnode_t *dir, const char *name, vnode_t **out)
{
    if (!name || !*name) return -ENOENT;

    sysfs_meta_t *dm = (sysfs_meta_t *)dir->fs_data;
    if (!dm) return -ENOENT;

    sf_type_t child_type = SF_ROOT;
    int child_idx = -1;

    if (dm->type == SF_ROOT && strcmp(name, "block") == 0) {
        child_type = SF_BLOCK;
    } else if (dm->type == SF_ROOT && strcmp(name, "class") == 0) {
        child_type = SF_CLASS;
    } else if (dm->type == SF_CLASS && strcmp(name, "drm") == 0) {
        child_type = SF_DRM;
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
    if (!vn) return -ENOMEM;
    memset(vn, 0, sizeof(*vn));

    int is_dir = (child_type == SF_ROOT || child_type == SF_BLOCK ||
                  child_type == SF_BLOCK_LOOP || child_type == SF_CLASS ||
                  child_type == SF_DRM || child_type == SF_DRM_CARD ||
                  child_type == SF_DRM_CONNECTOR || child_type == SF_DRM_CARD_DEVICE);

    vn->ino = (uint64_t)((child_type << 8) | ((child_idx + 1) & 0xFF));
    vn->type = is_dir ? VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    vnode_ref_init(vn, 1);
    vn->parent = dir;
    vnode_get(dir);
    vn->ops = dir->ops;

    sysfs_meta_t *meta = sysfs_meta_create(child_type, child_idx);
    if (meta) {
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
        }
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
    if (vn->fs_data) kfree(vn->fs_data);
    vnode_put(vn->parent);
    kfree(vn);
}

static vfile_t *sysfs_open_vnode(vnode_t *vn, int flags);

static vnode_ops_t g_sysfs_vnode_ops = {
    .lookup  = sysfs_lookup,
    .stat    = sysfs_stat,
    .open    = sysfs_open_vnode,
    .release = sysfs_release,
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

static int sysfs_freaddir(vfile_t *vf, void *dirp, size_t count)
{
    if (!vf || !vf->vnode) return -EBADF;
    sysfs_meta_t *dm = (sysfs_meta_t *)vf->vnode->fs_data;
    if (!dm) return -ENOENT;

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
    } else if (dm->type == SF_CLASS) {
        entries[nent].name = "drm"; entries[nent].dtype = 4; nent++;
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

static vfile_ops_t g_sysfs_fops = {
    .read    = sysfs_fread,
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
        meta ? meta->height : 0);
    if (!priv) { vfile_free(vf); return NULL; }
    vf->priv = priv;
    return vf;
}

/*
 * A20OS — ISO 9660 (CD-ROM) read-only filesystem.
 *
 * Parses the Primary Volume Descriptor at logical block 16, walks directory
 * records (with cross-block and multi-extent continuation handling), and
 * serves file data directly from the block cache.  Read-only by design: the
 * VFS mutation ops are left NULL so vfs_* returns the standard errno.
 *
 * The on-disk format is defined by ECMA-119 / ISO 9660.  The name
 * translation strategy (lowercase, strip ";1") follows the approach of
 * ViudiraTech/Uinxed-Kernel (Apache-2.0); this implementation is rewritten
 * for A20OS's own VFS / block-cache model.  See docs/ACKNOWLEDGMENTS.md.
 */
#include "fs/isofs.h"
#include "fs/file.h"
#include "fs/vfs.h"
#include "fs/block_cache.h"
#include "core/string.h"
#include "core/stdio.h"
#include "core/defs.h"
#include "core/klog.h"
#include "mm/mm.h"

#define ISOFS_MAX_NAME 255

/* ------------------------------------------------------------------ */
/* Name translation (ISO 9660 → plain name)                            */
/* ------------------------------------------------------------------ */

/* Lowercase, strip trailing ";1" version, map ';' and '/' to '.'. */
static void isofs_name_translate(const uint8_t *name, uint8_t name_len,
                                 char *out, size_t out_sz) {
    size_t len = name_len;
    if (len >= out_sz)
        len = out_sz - 1;
    size_t o = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = name[i];
        if (i + 1 == len - 1 && name[i] == ';' && name[i + 1] == '1')
            break;                      /* strip ";1" */
        if (c == ';' || c == '/')
            c = '.';
        if (c >= 'A' && c <= 'Z')
            c = (uint8_t)(c + 32);
        out[o++] = (char)c;
    }
    out[o] = '\0';
}

/* ------------------------------------------------------------------ */
/* Directory record iteration                                          */
/* ------------------------------------------------------------------ */

/* Read one directory record at logical offset `pos` within a directory.
 * Handles records that span block boundaries by assembling them into buf.
 * Returns record length (>0) and fills *rec; 0 = end of directory. */
static int isofs_read_dirent(isofs_sb_t *sb, uint32_t dir_extent,
                             uint64_t pos, uint8_t *buf, size_t buf_sz) {
    uint32_t bs = sb->block_size;
    uint64_t blk = (uint64_t)dir_extent + pos / bs;
    uint32_t off = (uint32_t)(pos % bs);

    if (bcache_read_bytes(sb->bc, blk * bs + off, buf, 1) < 0)
        return -EIO;
    uint8_t len = buf[0];
    if (len == 0)
        return 0;                       /* end of directory */
    if (len < (int)ISO_DIR_REC_HEADER)
        return len;                     /* invalid; skip */

    if (off + len <= bs) {
        /* Fits in this block. */
        if (bcache_read_bytes(sb->bc, blk * bs + off, buf, len) < 0)
            return -EIO;
        return len;
    }

    /* Cross-block: copy first fragment, then the remainder from blk+1. */
    size_t first = bs - off;
    if (first > buf_sz)
        first = buf_sz;
    if (bcache_read_bytes(sb->bc, blk * bs + off, buf, first) < 0)
        return -EIO;
    size_t rest = len - first;
    if (rest > buf_sz - first)
        rest = buf_sz - first;
    if (bcache_read_bytes(sb->bc, (blk + 1) * bs, buf + first, rest) < 0)
        return -EIO;
    return (int)len;
}

/* ------------------------------------------------------------------ */
/* Vnode factory                                                       */
/* ------------------------------------------------------------------ */

static vnode_ops_t g_isofs_vnode_ops;

static vnode_t *isofs_make_vnode(isofs_sb_t *sb, uint32_t extent,
                                 uint64_t size, int is_dir, vnode_t *parent) {
    vnode_t *vn = (vnode_t *)kmalloc(sizeof(vnode_t));
    if (!vn)
        return NULL;
    memset(vn, 0, sizeof(*vn));
    vn->ino = extent;
    vn->type = is_dir ? VFS_FT_DIR : VFS_FT_REGULAR;
    vn->mode = is_dir ? (S_IFDIR | 0555) : (S_IFREG | 0444);
    vn->uid = 0;
    vn->gid = 0;
    vn->size = (size_t)size;
    vnode_ref_init(vn, 1);
    vn->parent = parent;
    if (parent) {
        vnode_get(parent);
        vn->mnt = parent->mnt;
    }

    isofs_vnode_priv_t *fp = (isofs_vnode_priv_t *)kmalloc(sizeof(isofs_vnode_priv_t));
    if (!fp) {
        kfree(vn);
        return NULL;
    }
    fp->sb = sb;
    fp->extent = extent;
    fp->file_size = size;
    fp->is_dir = is_dir;
    vn->fs_data = fp;
    vn->ops = &g_isofs_vnode_ops;
    return vn;
}

/* ------------------------------------------------------------------ */
/* Directory scanning                                                  */
/* ------------------------------------------------------------------ */

/* Walk a directory, calling visit(rec, pos, ctx) for each entry.
 * Multi-extent (0x80) and non-first records are skipped. */
static int isofs_walk_dir(isofs_sb_t *sb, uint32_t dir_extent,
                          uint64_t dir_size,
                          int (*visit)(const iso_directory_record_t *rec,
                                       uint64_t pos, void *ctx),
                          void *ctx) {
    uint32_t bs = sb->block_size;
    uint8_t *buf = (uint8_t *)kmalloc(bs + 2048U);   /* room for cross-block */
    if (!buf)
        return -ENOMEM;

    uint64_t pos = 0;
    int result = 0;
    while (pos + (uint64_t)ISO_DIR_REC_HEADER <= dir_size) {
        int len = isofs_read_dirent(sb, dir_extent, pos, buf, (size_t)bs + 2048U);
        if (len < 0) {
            result = len;
            break;
        }
        if (len == 0) {
            /* Align to the next block boundary. */
            pos = (pos + bs) & ~((uint64_t)bs - 1);
            continue;
        }
        if (len < (int)ISO_DIR_REC_HEADER) {
            pos += (uint32_t)len;
            continue;
        }
        iso_directory_record_t *rec = (iso_directory_record_t *)buf;
        if (rec->flags & ISO_FLAG_MULTI_EXTENT) {
            pos += (uint32_t)len;       /* continuation record; skip */
            continue;
        }
        uint8_t name_len = rec->name_len;
        if (name_len <= 1 && (name_len == 0 || rec->name[0] <= 1)) {
            pos += (uint32_t)len;       /* "." or ".." */
            continue;
        }
        if (visit(rec, pos, ctx)) {
            result = 1;
            break;
        }
        pos += (uint32_t)len;
    }
    kfree(buf);
    return result;
}

typedef struct {
    const char *name;
    uint32_t extent;
    uint64_t size;
    int is_dir;
    int found;
} isofs_lookup_ctx_t;

static int isofs_lookup_visit(const iso_directory_record_t *rec,
                              uint64_t pos, void *ctx) {
    isofs_lookup_ctx_t *lc = (isofs_lookup_ctx_t *)ctx;
    char name[ISOFS_MAX_NAME];
    isofs_name_translate(rec->name, rec->name_len, name, sizeof(name));
    if (strcmp(name, lc->name) == 0) {
        lc->extent = iso_733(rec->extent) + rec->ext_attr_length;
        lc->size = iso_733(rec->size);
        lc->is_dir = (rec->flags & ISO_FLAG_DIRECTORY) != 0;
        lc->found = 1;
        return 1;
    }
    (void)pos;
    return 0;
}

/* ------------------------------------------------------------------ */
/* vnode ops                                                           */
/* ------------------------------------------------------------------ */

static int isofs_lookup(vnode_t *dir, const char *name, vnode_t **out) {
    isofs_vnode_priv_t *fp = (isofs_vnode_priv_t *)dir->fs_data;
    if (!fp || !fp->is_dir)
        return -ENOTDIR;

    isofs_lookup_ctx_t lc;
    memset(&lc, 0, sizeof(lc));
    lc.name = name;
    int r = isofs_walk_dir(fp->sb, fp->extent, fp->file_size,
                           isofs_lookup_visit, &lc);
    if (r < 0)
        return r;
    if (!lc.found)
        return -ENOENT;

    vnode_t *vn = isofs_make_vnode(fp->sb, lc.extent, lc.size, lc.is_dir, dir);
    if (!vn)
        return -ENOMEM;
    *out = vn;
    return 0;
}

static int isofs_stat(vnode_t *vn, kstat_t *st) {
    if (!vn || !st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->st_ino = vn->ino;
    st->st_size = (int64_t)vn->size;
    st->st_mode = vn->mode;
    st->st_uid = vn->uid;
    st->st_gid = vn->gid;
    st->st_blksize = 2048;
    st->st_blocks = (vn->size + 511) / 512;
    st->st_nlink = 1;
    return 0;
}

static int isofs_statfs(vnode_t *vn, kstatfs_t *st) {
    isofs_vnode_priv_t *fp = (isofs_vnode_priv_t *)vn->fs_data;
    if (!fp || !st)
        return -EINVAL;
    memset(st, 0, sizeof(*st));
    st->f_type = 0x9660;               /* ISOFS_SUPER_MAGIC */
    st->f_bsize = fp->sb->block_size;
    st->f_blocks = fp->sb->vol_space;
    st->f_bfree = 0;
    st->f_bavail = 0;
    st->f_files = 0;
    st->f_ffree = 0;
    st->f_namelen = 255;
    st->f_frsize = fp->sb->block_size;
    return 0;
}

static void isofs_release_vn(vnode_t *vn) {
    if (!vn)
        return;
    if (vn->fs_data) {
        kfree(vn->fs_data);
        vn->fs_data = NULL;
    }
    if (vn->parent && vn->parent != vn)
        vnode_put(vn->parent);
    kfree(vn);
}

/* ------------------------------------------------------------------ */
/* File operations                                                     */
/* ------------------------------------------------------------------ */

static int isofs_fread(vfile_t *vf, char *buf, size_t count) {
    isofs_fctx_t *fc = (isofs_fctx_t *)vf->priv;
    if (!fc || fc->is_dir)
        return -EISDIR;
    if (count == 0)
        return 0;
    if (fc->file_off >= fc->file_size)
        return 0;

    size_t remaining = fc->file_size - fc->file_off;
    if (count > remaining)
        count = remaining;

    uint64_t byte_off = (uint64_t)fc->extent * fc->sb->block_size + fc->file_off;
    int r = bcache_read_bytes(fc->sb->bc, byte_off, buf, count);
    if (r < 0)
        return r;
    fc->file_off += count;
    vf->offset = fc->file_off;
    return (int)count;
}

static long isofs_flseek(vfile_t *vf, long offset, int whence) {
    isofs_fctx_t *fc = (isofs_fctx_t *)vf->priv;
    long new_off;
    switch (whence) {
        case SEEK_SET: new_off = offset; break;
        case SEEK_CUR: new_off = (long)vf->offset + offset; break;
        case SEEK_END: new_off = (long)fc->file_size + offset; break;
        default: return -EINVAL;
    }
    if (new_off < 0)
        return -EINVAL;
    vf->offset = (size_t)new_off;
    fc->file_off = (size_t)new_off;
    return new_off;
}

/* getdents: iterate directory records, emitting vfs_dirent64_t entries. */
static int isofs_freaddir(vfile_t *vf, void *dirp, size_t count) {
    isofs_fctx_t *fc = (isofs_fctx_t *)vf->priv;
    if (!fc || !fc->is_dir)
        return -ENOTDIR;

    uint32_t bs = fc->sb->block_size;
    uint8_t *buf = (uint8_t *)kmalloc(bs + 2048U);
    if (!buf)
        return -ENOMEM;
    char *out = (char *)dirp;
    size_t total = 0;

    uint64_t pos = fc->dir_off;
    while (pos + (uint64_t)ISO_DIR_REC_HEADER <= fc->file_size) {
        int len = isofs_read_dirent(fc->sb, fc->extent, pos, buf,
                                    (size_t)bs + 2048U);
        if (len < 0)
            break;
        if (len == 0) {
            pos = (pos + bs) & ~((uint64_t)bs - 1);
            continue;
        }
        if (len < (int)ISO_DIR_REC_HEADER) {
            pos += (uint32_t)len;
            continue;
        }
        iso_directory_record_t *rec = (iso_directory_record_t *)buf;
        if (rec->flags & ISO_FLAG_MULTI_EXTENT) {
            pos += (uint32_t)len;
            continue;
        }
        uint8_t name_len = rec->name_len;
        if (name_len <= 1 && (name_len == 0 || rec->name[0] <= 1)) {
            pos += (uint32_t)len;
            continue;
        }

        char fname[ISOFS_MAX_NAME];
        isofs_name_translate(rec->name, name_len, fname, sizeof(fname));
        size_t nl = strlen(fname);
        size_t reclen = offsetof(vfs_dirent64_t, d_name) + nl + 1;
        reclen = (reclen + 7) & ~7UL;
        if (total + reclen > count)
            break;

        vfs_dirent64_t *dent = (vfs_dirent64_t *)(out + total);
        dent->d_ino = iso_733(rec->extent) + rec->ext_attr_length;
        dent->d_off = (int64_t)pos;
        dent->d_reclen = (uint16_t)reclen;
        dent->d_type = (rec->flags & ISO_FLAG_DIRECTORY) ? DT_DIR : DT_REG;
        memcpy(dent->d_name, fname, nl + 1);
        total += reclen;
        pos += (uint32_t)len;
        fc->dir_off = pos;
    }
    kfree(buf);
    return (int)total;
}

static int isofs_fclose(vfile_t *vf) {
    if (vf->priv) {
        kfree(vf->priv);
        vf->priv = NULL;
    }
    return 0;
}

static vfile_ops_t g_isofs_fops = {
    .read    = isofs_fread,
    .lseek   = isofs_flseek,
    .readdir = isofs_freaddir,
    .close   = isofs_fclose,
};

static vfile_t *isofs_open_vnode(vnode_t *vn, int flags) {
    isofs_vnode_priv_t *fp = (isofs_vnode_priv_t *)vn->fs_data;
    if (!fp)
        return NULL;

    isofs_fctx_t *fc = (isofs_fctx_t *)kmalloc(sizeof(isofs_fctx_t));
    if (!fc)
        return NULL;
    memset(fc, 0, sizeof(*fc));
    fc->sb = fp->sb;
    fc->extent = fp->extent;
    fc->file_size = fp->file_size;
    fc->is_dir = fp->is_dir;
    fc->dir_off = 0;

    vfile_t *vf = vfile_alloc();
    if (!vf) {
        kfree(fc);
        return NULL;
    }
    vf->vnode = vn;
    vnode_get(vn);
    vf->flags = flags;
    vf->offset = 0;
    vfile_ref_init(vf, 1);
    vf->ops = &g_isofs_fops;
    vf->priv = fc;
    return vf;
}

static int isofs_vn_readpage(vnode_t *vn, uint64_t index,
                             void *data, size_t len) {
    if (!vn || !vn->fs_data || !data)
        return -EINVAL;
    isofs_vnode_priv_t *fp = (isofs_vnode_priv_t *)vn->fs_data;
    if (fp->is_dir)
        return -EISDIR;

    memset(data, 0, len);
    uint64_t off = index * PAGE_SIZE;
    if (off >= fp->file_size)
        return 0;
    size_t n = fp->file_size - (size_t)off;
    if (n > len)
        n = len;
    uint64_t byte_off = (uint64_t)fp->extent * fp->sb->block_size + off;
    return bcache_read_bytes(fp->sb->bc, byte_off, data, n) < 0 ? -EIO : 0;
}

static vnode_ops_t g_isofs_vnode_ops = {
    .lookup   = isofs_lookup,
    .stat     = isofs_stat,
    .statfs   = isofs_statfs,
    .readpage = isofs_vn_readpage,
    .open     = isofs_open_vnode,
    .release  = isofs_release_vn,
};

/* ------------------------------------------------------------------ */
/* Mount / unmount                                                     */
/* ------------------------------------------------------------------ */

vnode_t *isofs_mount(bcache_t *bc) {
    isofs_sb_t *sb = (isofs_sb_t *)kmalloc(sizeof(isofs_sb_t));
    if (!sb)
        return NULL;
    memset(sb, 0, sizeof(*sb));
    sb->bc = bc;
    sb->block_size = ISOFS_BLOCK_SIZE;
    mutex_init(&sb->lock);

    /* Scan the Volume Descriptor Set (logical blocks 16..100). */
    iso_primary_descriptor_t *pvd =
        (iso_primary_descriptor_t *)kmalloc(ISOFS_BLOCK_SIZE);
    if (!pvd) {
        kfree(sb);
        return NULL;
    }

    int found = 0;
    for (uint32_t blk = ISOFS_VD_OFFSET; blk < 100U; blk++) {
        if (bcache_read_bytes(bc, (uint64_t)blk * ISOFS_BLOCK_SIZE,
                              pvd, ISOFS_BLOCK_SIZE) < 0)
            break;
        if (memcmp(pvd->id, ISO9660_ID, 5) != 0)
            continue;
        if (pvd->type == ISO_VD_PRIMARY) {
            found = 1;
            break;
        }
        if (pvd->type == ISO_VD_TERMINATOR)
            break;
    }
    if (!found) {
        kfree(pvd);
        kfree(sb);
        kdebug("[ISOFS] no primary volume descriptor\n");
        return NULL;
    }

    uint32_t lb_size = iso_721(pvd->logical_block_size);
    if (lb_size >= 512 && lb_size <= 4096 &&
        (lb_size & (lb_size - 1)) == 0)
        sb->block_size = lb_size;

    sb->vol_space = iso_733(pvd->volume_space_size);

    iso_directory_record_t *root = (iso_directory_record_t *)pvd->root_directory_record;
    sb->root_extent = iso_733(root->extent) + root->ext_attr_length;
    sb->root_size = iso_733(root->size);

    printf("[ISOFS] Mounted: block_size=%u vol_blocks=%u root_extent=%u root_size=%u\n",
           sb->block_size, sb->vol_space, sb->root_extent, (uint32_t)sb->root_size);

    kfree(pvd);

    vnode_t *root_vn = isofs_make_vnode(sb, sb->root_extent, sb->root_size, 1, NULL);
    if (!root_vn) {
        kfree(sb);
        return NULL;
    }
    root_vn->parent = root_vn;
    return root_vn;
}

void isofs_unmount(vnode_t *root) {
    if (!root || !root->fs_data)
        return;
    isofs_vnode_priv_t *fp = (isofs_vnode_priv_t *)root->fs_data;
    isofs_sb_t *sb = fp->sb;
    bcache_sync(sb->bc);

    if (root->ops && root->ops->release)
        root->ops->release(root);
    kfree(sb);
}

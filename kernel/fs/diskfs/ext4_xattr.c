/*
 * ext4 in-inode extended attributes.
 *
 * ext4 keeps small xattrs in the tail of a 256-byte inode: after the 128-byte
 * base inode and i_extra_isize bytes of extra fields sits a header
 * (EXT4_EA_MAGIC), a list of entries growing forward, and the values growing
 * backward from the end of the inode.  An inode whose header magic is absent
 * simply has no attributes yet.
 *
 * Layout of one entry (16 bytes, then the name padded to 4):
 *
 *   0  u8  e_name_len
 *   1  u8  e_name_index   (1 = user., 2/3 = system.posix_acl_{access,default})
 *   2  u16 e_value_offs   relative to the first entry, NOT to the inode
 *   4  u32 e_value_inum   non-zero = value lives in a separate inode (unused)
 *   8  u32 e_value_size
 *  12  u8  e_name[]
 *
 * The last entry is followed by four zero bytes.  Values are 4-byte aligned
 * and placed top-down.
 *
 * The driver used to stamp i_extra_isize with the whole tail (128), which
 * leaves no attribute space at all; it now uses the mke2fs value so the tail
 * is laid out the way e2fsprogs expects.
 *
 * Every mutation rebuilds the attribute area in a scratch copy of the inode
 * and writes it once, so a failure to fit leaves the on-disk inode untouched
 * rather than half-updated.
 */

#include "fs/ext4.h"
#include "fs/ext4_internal.h"
#include "fs/vfs.h"
#include "fs/xattr.h"
#include "fs/block_cache.h"
#include "core/string.h"
#include "core/lock.h"
#include "mm/slab.h"

#define EXT4_EA_MAGIC        0xEA020000U
#define EXT4_EA_HDR_SIZE     4
#define EXT4_EA_ENTRY_SIZE   16
#define EXT4_EA_INDEX_USER   1
#define EXT4_EA_INDEX_ACL_ACCESS  2
#define EXT4_EA_INDEX_ACL_DEFAULT 3

#define EXT4_EA_NAME_MAX     128

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void wr16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static size_t align4(size_t n)
{
    return (n + 3u) & ~(size_t)3u;
}

static uint64_t inode_byte_offset(ext4_sb_info_t *sb, uint32_t ino)
{
    uint32_t g = (ino - 1) / sb->inodes_per_group;
    uint32_t i = (ino - 1) % sb->inodes_per_group;
    uint64_t table = (uint64_t)sb->group_descs[g].bg_inode_table_lo;
    return table * sb->block_size + (uint64_t)i * sb->inode_size;
}

static int inode_raw_read(ext4_sb_info_t *sb, uint32_t ino, uint8_t *raw)
{
    if (ino < 1 || ino > sb->inodes_count)
        return -EINVAL;
    if (inode_byte_offset(sb, ino) == 0)
        return -EIO;
    if (bcache_read_bytes(sb->bc, inode_byte_offset(sb, ino), raw,
                          sb->inode_size) < 0)
        return -EIO;
    return 0;
}

static int inode_raw_write(ext4_sb_info_t *sb, uint32_t ino, uint8_t *raw)
{
    if (ext4_has_metadata_csum(sb))
        ext4_inode_checksum_put(sb, ino, raw, sb->inode_size);
    if (ext4_meta_write(sb, inode_byte_offset(sb, ino), raw,
                        sb->inode_size) < 0)
        return -EIO;
    return 0;
}

/* Map the VFS spelling to ext4's (name_index, suffix).  Only the namespaces
 * this driver can store durably are claimed; anything else is left to the
 * VFS's volatile table. */
static int ea_split_name(const char *name, uint8_t *index, const char **suffix)
{
    static const struct {
        const char *prefix;
        uint8_t index;
    } map[] = {
        { "user.", EXT4_EA_INDEX_USER },
        { "system.posix_acl_access", EXT4_EA_INDEX_ACL_ACCESS },
        { "system.posix_acl_default", EXT4_EA_INDEX_ACL_DEFAULT },
    };
    for (size_t i = 0; i < sizeof(map) / sizeof(map[0]); i++) {
        size_t n = strlen(map[i].prefix);
        if (strncmp(name, map[i].prefix, n) == 0) {
            *index = map[i].index;
            /* ACL entries carry no suffix: the whole name IS the attribute. */
            *suffix = (map[i].index == EXT4_EA_INDEX_USER) ? name + n : "";
            return 0;
        }
    }
    return -EOPNOTSUPP;
}

static const char *ea_index_prefix(uint8_t index)
{
    switch (index) {
    case EXT4_EA_INDEX_USER: return "user.";
    case EXT4_EA_INDEX_ACL_ACCESS: return "system.posix_acl_access";
    case EXT4_EA_INDEX_ACL_DEFAULT: return "system.posix_acl_default";
    default: return NULL;
    }
}

/* Where the attribute area starts, and how much of the inode it may use. */
static int ea_area(ext4_sb_info_t *sb, const uint8_t *raw, size_t *out_start)
{
    if (sb->inode_size <= EXT4_INODE_SIZE_STATIC)
        return -EOPNOTSUPP;
    uint16_t extra = rd16(raw + offsetof(ext4_inode_t, i_extra_isize));
    size_t start = EXT4_INODE_SIZE_STATIC + extra;
    if (extra == 0 || start + EXT4_EA_HDR_SIZE + EXT4_EA_ENTRY_SIZE >
        sb->inode_size)
        return -EOPNOTSUPP;
    *out_start = start;
    return 0;
}

struct ea_scan {
    size_t start;      /* the magic */
    size_t first;      /* first entry */
    size_t end;        /* end of the entry list (the terminator) */
    size_t value_low;  /* lowest value offset; inode_size when there are none */
    int    initialized;
};

static int ea_walk(ext4_sb_info_t *sb, const uint8_t *raw, struct ea_scan *sc)
{
    int r = ea_area(sb, raw, &sc->start);
    if (r < 0)
        return r;
    sc->first = sc->start + EXT4_EA_HDR_SIZE;
    sc->end = sc->first;
    sc->value_low = sb->inode_size;
    sc->initialized = rd32(raw + sc->start) == EXT4_EA_MAGIC;
    if (!sc->initialized)
        return 0;

    size_t off = sc->first;
    while (off + EXT4_EA_ENTRY_SIZE <= sb->inode_size) {
        const uint8_t *e = raw + off;
        if (rd32(e) == 0)
            break;
        uint8_t nlen = e[0];
        size_t rec = EXT4_EA_ENTRY_SIZE + align4(nlen);
        if (off + rec > sb->inode_size)
            return -EIO;
        uint32_t vsize = rd32(e + 8);
        if (rd32(e + 4) != 0)
            return -EOPNOTSUPP;      /* value stored in another inode */
        if (vsize) {
            size_t va = sc->first + rd16(e + 2);
            if (va + vsize > sb->inode_size)
                return -EIO;
            if (va < sc->value_low)
                sc->value_low = va;
        }
        off += rec;
    }
    sc->end = off;
    return 0;
}

/* Find @index/@suffix; returns its entry offset or -ENODATA. */
static int64_t ea_find(const uint8_t *raw, const struct ea_scan *sc,
                       uint8_t index, const char *suffix)
{
    size_t slen = strlen(suffix);
    for (size_t off = sc->first; off + EXT4_EA_ENTRY_SIZE <= sc->end; ) {
        const uint8_t *e = raw + off;
        if (rd32(e) == 0)
            break;
        uint8_t nlen = e[0];
        if (e[1] == index && nlen == slen &&
            memcmp(e + EXT4_EA_ENTRY_SIZE, suffix, slen) == 0)
            return (int64_t)off;
        off += EXT4_EA_ENTRY_SIZE + align4(nlen);
    }
    return -ENODATA;
}

/*
 * Rebuild the whole attribute area with @drop removed (if non-NULL) and
 * @add inserted.  Everything is laid out fresh in @tmp, then copied back, so
 * the result is either complete or not applied at all.
 */
static int ea_rebuild(ext4_sb_info_t *sb, const uint8_t *raw, uint8_t *tmp,
                      const struct ea_scan *sc,
                      uint8_t drop_index, const char *drop_name,
                      uint8_t add_index, const char *add_name,
                      const void *add_value, size_t add_size)
{
    size_t cursor = sc->first;      /* entry write position */
    size_t value = sb->inode_size;  /* values grow down from here */

    memset(tmp + sc->first, 0, sb->inode_size - sc->first);
    wr32(tmp + sc->start, EXT4_EA_MAGIC);

    /* Existing entries, in their original order. */
    for (size_t off = sc->first; sc->initialized &&
         off + EXT4_EA_ENTRY_SIZE <= sc->end; ) {
        const uint8_t *e = raw + off;
        if (rd32(e) == 0)
            break;
        uint8_t nlen = e[0];
        size_t rec = EXT4_EA_ENTRY_SIZE + align4(nlen);
        size_t vsize = rd32(e + 8);
        int drop = drop_name && e[1] == drop_index && nlen == strlen(drop_name) &&
                   memcmp(e + EXT4_EA_ENTRY_SIZE, drop_name, nlen) == 0;
        if (drop) {
            off += rec;
            continue;
        }
        /* The new value may be larger than the old one; both are placed at
         * the end so the space check below sees the real requirement. */
        size_t need = rec + align4(vsize);
        if (cursor + need > value)
            return -ENOSPC;
        size_t vo = value - align4(vsize);
        if (vsize)
            memcpy(tmp + vo, raw + sc->first + rd16(e + 2), vsize);
        uint8_t *d = tmp + cursor;
        memcpy(d, e, rec);
        if (vsize) {
            wr16(d + 2, (uint16_t)(vo - sc->first));
            value = vo;
        } else {
            wr16(d + 2, 0);
        }
        cursor += rec;
        off += rec;
    }

    if (add_name) {
        size_t nlen = strlen(add_name);
        if (nlen > EXT4_EA_NAME_MAX)
            return -ERANGE;
        size_t rec = EXT4_EA_ENTRY_SIZE + align4(nlen);
        size_t need = rec + align4(add_size);
        if (cursor + need > value)
            return -ENOSPC;
        size_t vo = value - align4(add_size);
        if (add_size)
            memcpy(tmp + vo, add_value, add_size);
        uint8_t *d = tmp + cursor;
        memset(d, 0, rec);
        d[0] = (uint8_t)nlen;
        d[1] = add_index;
        wr16(d + 2, add_size ? (uint16_t)(vo - sc->first) : 0);
        wr32(d + 4, 0);
        wr32(d + 8, (uint32_t)add_size);
        memcpy(d + EXT4_EA_ENTRY_SIZE, add_name, nlen);
        cursor += rec;
        value = vo;
    }

    /* The list terminator is the first four bytes of an entry all zero.  A
 * zero-length *name* is not the terminator: ACL entries are stored with
 * e_name_len 0 and are identified by their name_index alone. */
    if (cursor + 4 > value)
        return -ENOSPC;
    memset(tmp + cursor, 0, 4);
    return 0;
}

/* Shared mutation helper: read, rebuild, write under the metadata lock. */
static int ea_mutate(vnode_t *vn, uint8_t drop_index, const char *drop_name,
                     uint8_t add_index, const char *add_name,
                     const void *add_value, size_t add_size)
{
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (!p || !p->sb)
        return -EIO;
    ext4_sb_info_t *sb = p->sb;
    if (p->type == VFS_FT_DIR && add_name && add_index != EXT4_EA_INDEX_ACL_ACCESS &&
        add_index != EXT4_EA_INDEX_ACL_DEFAULT)
        return -EPERM;   /* user.* on a directory is not allowed by ext4 */

    uint8_t *raw = (uint8_t *)kmalloc(sb->inode_size);
    uint8_t *tmp = (uint8_t *)kmalloc(sb->inode_size);
    if (!raw || !tmp) {
        if (raw) kfree(raw);
        if (tmp) kfree(tmp);
        return -ENOMEM;
    }
    rw_mutex_write_lock(&sb->metadata_lock);
    int ret = inode_raw_read(sb, p->inode_num, raw);
    if (ret == 0) {
        struct ea_scan sc;
        ret = ea_walk(sb, raw, &sc);
        if (ret == 0) {
            memcpy(tmp, raw, sb->inode_size);
            ret = ea_rebuild(sb, raw, tmp, &sc, drop_index, drop_name,
                             add_index, add_name, add_value, add_size);
            if (ret == 0)
                ret = inode_raw_write(sb, p->inode_num, tmp);
        }
    }
    rw_mutex_write_unlock(&sb->metadata_lock);
    kfree(raw);
    kfree(tmp);
    return ret;
}

int ext4_xattr_get(vnode_t *vn, const char *name, void *value, size_t size)
{
    uint8_t index;
    const char *suffix;
    int r = ea_split_name(name, &index, &suffix);
    if (r < 0)
        return r;
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (!p || !p->sb)
        return -EIO;
    ext4_sb_info_t *sb = p->sb;

    uint8_t *raw = (uint8_t *)kmalloc(sb->inode_size);
    if (!raw)
        return -ENOMEM;
    rw_mutex_write_lock(&sb->metadata_lock);
    r = inode_raw_read(sb, p->inode_num, raw);
    struct ea_scan sc;
    if (r == 0)
        r = ea_walk(sb, raw, &sc);
    int64_t entry = r == 0 ? ea_find(raw, &sc, index, suffix) : r;
    if (entry < 0) {
        rw_mutex_write_unlock(&sb->metadata_lock);
        kfree(raw);
        return (int)entry == -ENODATA ? -ENODATA : (int)entry;
    }
    const uint8_t *e = raw + entry;
    size_t vsize = rd32(e + 8);
    size_t have = vsize;
    if (value && size > 0) {
        if (size < vsize) {
            rw_mutex_write_unlock(&sb->metadata_lock);
            kfree(raw);
            return -ERANGE;
        }
        if (vsize)
            memcpy(value, raw + sc.first + rd16(e + 2), vsize);
    }
    rw_mutex_write_unlock(&sb->metadata_lock);
    kfree(raw);
    return (int)have;
}

int ext4_xattr_set(vnode_t *vn, const char *name, const void *value,
                   size_t size, int flags)
{
    uint8_t index;
    const char *suffix;
    int r = ea_split_name(name, &index, &suffix);
    if (r < 0)
        return r;

    /* XATTR_CREATE/REPLACE have to be decided from the on-disk state. */
    int64_t existing = ext4_xattr_get(vn, name, NULL, 0);
    if (existing < 0 && existing != -ENODATA)
        return (int)existing;
    if ((flags & XATTR_CREATE) && existing >= 0)
        return -EEXIST;
    if ((flags & XATTR_REPLACE) && existing < 0)
        return -ENODATA;

    return ea_mutate(vn, index, suffix, index, suffix, value, size);
}

int ext4_xattr_remove(vnode_t *vn, const char *name)
{
    uint8_t index;
    const char *suffix;
    int r = ea_split_name(name, &index, &suffix);
    if (r < 0)
        return r;
    int64_t existing = ext4_xattr_get(vn, name, NULL, 0);
    if (existing < 0)
        return (int)existing;
    return ea_mutate(vn, index, suffix, index, NULL, NULL, 0);
}

int ext4_xattr_list(vnode_t *vn, char *list, size_t size)
{
    ext4_vnode_priv_t *p = (ext4_vnode_priv_t *)vn->fs_data;
    if (!p || !p->sb)
        return -EIO;
    ext4_sb_info_t *sb = p->sb;

    uint8_t *raw = (uint8_t *)kmalloc(sb->inode_size);
    if (!raw)
        return -ENOMEM;
    rw_mutex_write_lock(&sb->metadata_lock);
    int r = inode_raw_read(sb, p->inode_num, raw);
    struct ea_scan sc;
    if (r == 0)
        r = ea_walk(sb, raw, &sc);
    if (r < 0) {
        rw_mutex_write_unlock(&sb->metadata_lock);
        kfree(raw);
        return r;
    }

    size_t total = 0;
    for (size_t off = sc.first; sc.initialized &&
         off + EXT4_EA_ENTRY_SIZE <= sc.end; ) {
        const uint8_t *e = raw + off;
        if (rd32(e) == 0)
            break;
        uint8_t nlen = e[0];
        const char *prefix = ea_index_prefix(e[1]);
        if (prefix)
            total += strlen(prefix) + nlen + 1;
        else if (e[1] != 0)
            total += nlen + 1;      /* unknown index: surface the raw name */
        off += EXT4_EA_ENTRY_SIZE + align4(nlen);
    }
    if (!list || size == 0) {
        rw_mutex_write_unlock(&sb->metadata_lock);
        kfree(raw);
        return (int)total;
    }
    if (size < total) {
        rw_mutex_write_unlock(&sb->metadata_lock);
        kfree(raw);
        return -ERANGE;
    }
    size_t out = 0;
    for (size_t off = sc.first; sc.initialized &&
         off + EXT4_EA_ENTRY_SIZE <= sc.end; ) {
        const uint8_t *e = raw + off;
        if (rd32(e) == 0)
            break;
        uint8_t nlen = e[0];
        const char *prefix = ea_index_prefix(e[1]);
        if (prefix) {
            size_t pl = strlen(prefix);
            memcpy(list + out, prefix, pl);
            out += pl;
        }
        memcpy(list + out, e + EXT4_EA_ENTRY_SIZE, nlen);
        out += nlen;
        list[out++] = '\0';
        off += EXT4_EA_ENTRY_SIZE + align4(nlen);
    }
    rw_mutex_write_unlock(&sb->metadata_lock);
    kfree(raw);
    return (int)out;
}

#include "fs/xattr.h"

#include "core/consts.h"
#include "core/string.h"

#define XATTR_TABLE_MAX 128

typedef struct {
    int used;
    void *mnt;
    uint64_t ino;
    char name[XATTR_NAME_MAX_LOCAL];
    size_t size;
    uint8_t value[XATTR_VALUE_MAX_LOCAL];
} xattr_store_t;

static xattr_store_t g_xattrs[XATTR_TABLE_MAX];

static int xattr_vnode_key(vnode_t *vn, void **mnt, uint64_t *ino)
{
    if (!vn || !mnt || !ino) return -ENOENT;
    *mnt = vn->mnt;
    *ino = vn->ino;
    return 0;
}

static int xattr_store_find(void *mnt, uint64_t ino, const char *name)
{
    for (int i = 0; i < XATTR_TABLE_MAX; i++) {
        if (g_xattrs[i].used && g_xattrs[i].mnt == mnt &&
            g_xattrs[i].ino == ino && strcmp(g_xattrs[i].name, name) == 0)
            return i;
    }
    return -1;
}

static int xattr_store_slot(void)
{
    for (int i = 0; i < XATTR_TABLE_MAX; i++)
        if (!g_xattrs[i].used) return i;
    return -ENOSPC;
}

static int xattr_check_name(const char *name)
{
    if (!name || !name[0]) return -EINVAL;
    if (strlen(name) >= XATTR_NAME_MAX_LOCAL) return -ERANGE;
    return 0;
}

int64_t xattr_set_vnode(vnode_t *vn, const char *name,
                        const void *value, size_t size, int flags)
{
    if (!vn) return -ENOENT;
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) return -EINVAL;
    int nr = xattr_check_name(name);
    if (nr < 0) return nr;
    if (size > XATTR_VALUE_MAX_LOCAL) return -ENOSPC;
    if (size && !value) return -EINVAL;

    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    int idx = xattr_store_find(mnt, ino, name);
    if ((flags & XATTR_CREATE) && idx >= 0) return -EEXIST;
    if ((flags & XATTR_REPLACE) && idx < 0) return -ENODATA;
    if (idx < 0) {
        idx = xattr_store_slot();
        if (idx < 0) return idx;
        memset(&g_xattrs[idx], 0, sizeof(g_xattrs[idx]));
        g_xattrs[idx].used = 1;
        g_xattrs[idx].mnt = mnt;
        g_xattrs[idx].ino = ino;
        strncpy(g_xattrs[idx].name, name, XATTR_NAME_MAX_LOCAL - 1);
    }
    if (size)
        memcpy(g_xattrs[idx].value, value, size);
    g_xattrs[idx].size = size;
    return 0;
}

int64_t xattr_get_vnode(vnode_t *vn, const char *name,
                        void *value, size_t size)
{
    if (!vn) return -ENOENT;
    int nr = xattr_check_name(name);
    if (nr < 0) return nr;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    int idx = xattr_store_find(mnt, ino, name);
    if (idx < 0) return -ENODATA;
    if (!value || size == 0) return (int64_t)g_xattrs[idx].size;
    if (size < g_xattrs[idx].size) return -ERANGE;
    memcpy(value, g_xattrs[idx].value, g_xattrs[idx].size);
    return (int64_t)g_xattrs[idx].size;
}

int64_t xattr_list_vnode(vnode_t *vn, char *list, size_t size)
{
    if (!vn) return -ENOENT;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    size_t total = 0;
    for (int i = 0; i < XATTR_TABLE_MAX; i++) {
        if (g_xattrs[i].used && g_xattrs[i].mnt == mnt && g_xattrs[i].ino == ino)
            total += strlen(g_xattrs[i].name) + 1;
    }
    if (!list || size == 0) return (int64_t)total;
    if (size < total) return -ERANGE;
    size_t off = 0;
    for (int i = 0; i < XATTR_TABLE_MAX; i++) {
        if (g_xattrs[i].used && g_xattrs[i].mnt == mnt && g_xattrs[i].ino == ino) {
            size_t len = strlen(g_xattrs[i].name) + 1;
            memcpy(list + off, g_xattrs[i].name, len);
            off += len;
        }
    }
    return (int64_t)total;
}

int64_t xattr_remove_vnode(vnode_t *vn, const char *name)
{
    if (!vn) return -ENOENT;
    int nr = xattr_check_name(name);
    if (nr < 0) return nr;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    int idx = xattr_store_find(mnt, ino, name);
    if (idx < 0) return -ENODATA;
    memset(&g_xattrs[idx], 0, sizeof(g_xattrs[idx]));
    return 0;
}

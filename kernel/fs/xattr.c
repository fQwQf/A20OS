#include "fs/xattr.h"

#include "core/consts.h"
#include "core/lock.h"
#include "core/string.h"
#include "proc/proc.h"

#define XATTR_TABLE_MAX 1024
#define XATTR_BUCKETS 64

typedef struct {
    int used;
    int next;                 /* index+1 of the next entry in this vnode chain,
                               * 0 = end of chain */
    void *mnt;
    uint64_t ino;
    char name[XATTR_NAME_MAX_LOCAL];
    size_t size;
    uint8_t value[XATTR_VALUE_MAX_LOCAL];
} xattr_store_t;

static xattr_store_t g_xattrs[XATTR_TABLE_MAX];
/* Bucket heads hold index+1 so that the zero-initialised table needs no
 * run-time setup and 0 unambiguously means "empty chain". */
static int g_xattr_heads[XATTR_BUCKETS];

/*
 * The table is shared by every task, and xattr_cleanup_vnode() runs from
 * vnode_put() on the last reference of a vnode while another CPU may be
 * inside xattr_get_vnode() copying that entry's value out.  g_xattr_lock is
 * a leaf: every critical section below is pure memory work (no allocation, no
 * VFS call, no copy_to_user), so it sits at the innermost end of the order in
 * core/lock.h and is never acquired while holding another lock.  Entries are
 * chained per (mnt, ino) instead of scanned, so lookup, listing and vnode
 * teardown are proportional to the number of attributes the inode actually
 * carries rather than to the table size.
 */
static spinlock_t g_xattr_lock = SPINLOCK_INIT;

static int xattr_vnode_key(vnode_t *vn, void **mnt, uint64_t *ino)
{
    if (!vn || !mnt || !ino) return -ENOENT;
    *mnt = vn->mnt;
    *ino = vn->ino;
    return 0;
}

static unsigned xattr_bucket(void *mnt, uint64_t ino)
{
    uint64_t h = (uint64_t)(uintptr_t)mnt;
    h ^= ino * 0x9E3779B97F4A7C15ULL;
    h ^= h >> 29;
    return (unsigned)(h & (XATTR_BUCKETS - 1));
}

static int xattr_store_find(void *mnt, uint64_t ino, const char *name)
{
    for (int link = g_xattr_heads[xattr_bucket(mnt, ino)]; link > 0;
         link = g_xattrs[link - 1].next) {
        int i = link - 1;
        if (g_xattrs[i].mnt == mnt && g_xattrs[i].ino == ino &&
            strcmp(g_xattrs[i].name, name) == 0)
            return i;
    }
    return -1;
}

static void xattr_store_link(int idx, void *mnt, uint64_t ino)
{
    unsigned b = xattr_bucket(mnt, ino);
    g_xattrs[idx].next = g_xattr_heads[b];
    g_xattr_heads[b] = idx + 1;
}

static void xattr_store_unlink(int idx)
{
    unsigned b = xattr_bucket(g_xattrs[idx].mnt, g_xattrs[idx].ino);
    int *pp = &g_xattr_heads[b];
    while (*pp > 0) {
        if (*pp - 1 == idx) {
            *pp = g_xattrs[idx].next;
            return;
        }
        pp = &g_xattrs[*pp - 1].next;
    }
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

int xattr_check_namespace(const char *name, int *needs_cap)
{
    int r = xattr_check_name(name);
    if (r < 0) return r;
    if (needs_cap) *needs_cap = 0;
    if (strncmp(name, XATTR_USER_PREFIX, strlen(XATTR_USER_PREFIX)) == 0) return 0;
    if (strncmp(name, XATTR_SYSTEM_PREFIX, strlen(XATTR_SYSTEM_PREFIX)) == 0) return 0;
    if (strncmp(name, XATTR_TRUSTED_PREFIX, strlen(XATTR_TRUSTED_PREFIX)) == 0) {
        if (needs_cap) *needs_cap = 1;
        return 0;
    }
    if (strncmp(name, XATTR_SECURITY_PREFIX, strlen(XATTR_SECURITY_PREFIX)) == 0) {
        if (needs_cap) *needs_cap = 1;
        return 0;
    }
    return -EINVAL;
}

int64_t xattr_set_vnode(vnode_t *vn, const char *name,
                        const void *value, size_t size, int flags)
{
    if (!vn) return -ENOENT;
    if (flags & ~(XATTR_CREATE | XATTR_REPLACE)) return -EINVAL;
    int needs_cap = 0;
    int nr = xattr_check_namespace(name, &needs_cap);
    if (nr < 0) return nr;
    if (needs_cap && (!proc_current() || !proc_has_cap(proc_current(), CAP_SYS_ADMIN)))
        return -EPERM;
    if (size > XATTR_VALUE_MAX_LOCAL) return -ENOSPC;
    if (size && !value) return -EINVAL;

    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    spin_lock(&g_xattr_lock);
    int idx = xattr_store_find(mnt, ino, name);
    if ((flags & XATTR_CREATE) && idx >= 0) { spin_unlock(&g_xattr_lock); return -EEXIST; }
    if ((flags & XATTR_REPLACE) && idx < 0) { spin_unlock(&g_xattr_lock); return -ENODATA; }
    if (idx < 0) {
        idx = xattr_store_slot();
        if (idx < 0) { spin_unlock(&g_xattr_lock); return idx; }
        memset(&g_xattrs[idx], 0, sizeof(g_xattrs[idx]));
        g_xattrs[idx].used = 1;
        g_xattrs[idx].mnt = mnt;
        g_xattrs[idx].ino = ino;
        strncpy(g_xattrs[idx].name, name, XATTR_NAME_MAX_LOCAL - 1);
        xattr_store_link(idx, mnt, ino);
    }
    if (size)
        memcpy(g_xattrs[idx].value, value, size);
    g_xattrs[idx].size = size;
    spin_unlock(&g_xattr_lock);
    return 0;
}

int64_t xattr_get_vnode(vnode_t *vn, const char *name,
                        void *value, size_t size)
{
    if (!vn) return -ENOENT;
    int nr = xattr_check_namespace(name, NULL);
    if (nr < 0) return nr;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    spin_lock(&g_xattr_lock);
    int idx = xattr_store_find(mnt, ino, name);
    if (idx < 0) { spin_unlock(&g_xattr_lock); return -ENODATA; }
    int64_t have = (int64_t)g_xattrs[idx].size;
    if (value && size > 0) {
        if (size < g_xattrs[idx].size) have = -ERANGE;
        else memcpy(value, g_xattrs[idx].value, g_xattrs[idx].size);
    }
    spin_unlock(&g_xattr_lock);
    return have;
}

int64_t xattr_list_vnode(vnode_t *vn, char *list, size_t size)
{
    if (!vn) return -ENOENT;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    spin_lock(&g_xattr_lock);
    size_t total = 0;
    for (int link = g_xattr_heads[xattr_bucket(mnt, ino)]; link > 0;
         link = g_xattrs[link - 1].next)
        total += strlen(g_xattrs[link - 1].name) + 1;
    if (!list || size == 0) { spin_unlock(&g_xattr_lock); return (int64_t)total; }
    if (size < total) { spin_unlock(&g_xattr_lock); return -ERANGE; }
    size_t off = 0;
    for (int link = g_xattr_heads[xattr_bucket(mnt, ino)]; link > 0;
         link = g_xattrs[link - 1].next) {
        size_t len = strlen(g_xattrs[link - 1].name) + 1;
        memcpy(list + off, g_xattrs[link - 1].name, len);
        off += len;
    }
    spin_unlock(&g_xattr_lock);
    return (int64_t)total;
}

int64_t xattr_remove_vnode(vnode_t *vn, const char *name)
{
    if (!vn) return -ENOENT;
    int needs_cap = 0;
    int nr = xattr_check_namespace(name, &needs_cap);
    if (nr < 0) return nr;
    if (needs_cap && (!proc_current() || !proc_has_cap(proc_current(), CAP_SYS_ADMIN)))
        return -EPERM;
    void *mnt;
    uint64_t ino;
    xattr_vnode_key(vn, &mnt, &ino);
    spin_lock(&g_xattr_lock);
    int idx = xattr_store_find(mnt, ino, name);
    if (idx < 0) { spin_unlock(&g_xattr_lock); return -ENODATA; }
    xattr_store_unlink(idx);
    memset(&g_xattrs[idx], 0, sizeof(g_xattrs[idx]));
    spin_unlock(&g_xattr_lock);
    return 0;
}

void xattr_cleanup_vnode(vnode_t *vn)
{
    if (!vn) return;
    void *mnt = vn->mnt;
    uint64_t ino = vn->ino;
    spin_lock(&g_xattr_lock);
    int *pp = &g_xattr_heads[xattr_bucket(mnt, ino)];
    while (*pp > 0) {
        int idx = *pp - 1;
        if (g_xattrs[idx].mnt == mnt && g_xattrs[idx].ino == ino) {
            *pp = g_xattrs[idx].next;
            memset(&g_xattrs[idx], 0, sizeof(g_xattrs[idx]));
            continue;
        }
        pp = &g_xattrs[idx].next;
    }
    spin_unlock(&g_xattr_lock);
}

#include "fs/vfs/dcache.h"
#include "core/lock.h"
#include "core/string.h"

/*
 * Small component lookup cache. It deliberately caches only stable
 * disk-backed/tmpfs lookups and is fully invalidated on namespace changes.
 */
#define VFS_DCACHE_MAX 128

typedef struct {
    int used;
    mount_t *mnt;
    uint64_t parent_ino;
    char name[MAX_NAME_LEN];
    vnode_t *vn;
    uint64_t age;
} vfs_dcache_entry_t;

static spinlock_t g_dcache_lock = SPINLOCK_INIT;
static vfs_dcache_entry_t g_dcache[VFS_DCACHE_MAX];
static uint64_t g_dcache_age;

static int vfs_dcache_enabled_for(vnode_t *dir)
{
    if (!dir || !dir->mnt)
        return 0;
    return dir->mnt->type == FS_TYPE_RAMFS ||
           dir->mnt->type == FS_TYPE_FAT32 ||
           dir->mnt->type == FS_TYPE_EXT4;
}

vnode_t *vfs_dcache_lookup(vnode_t *dir, const char *name)
{
    if (!vfs_dcache_enabled_for(dir) || !name || !*name)
        return NULL;

    spin_lock(&g_dcache_lock);
    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0 && e->vn && vnode_ref_read(e->vn) > 0) {
            e->age = ++g_dcache_age;
            vnode_get(e->vn);
            vnode_t *vn = e->vn;
            spin_unlock(&g_dcache_lock);
            return vn;
        }
    }
    spin_unlock(&g_dcache_lock);
    return NULL;
}

void vfs_dcache_insert(vnode_t *dir, const char *name, vnode_t *vn)
{
    if (!vfs_dcache_enabled_for(dir) || !name || !*name || !vn)
        return;

    spin_lock(&g_dcache_lock);
    int slot = -1;
    uint64_t oldest = (uint64_t)-1;
    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0) {
            e->age = ++g_dcache_age;
            spin_unlock(&g_dcache_lock);
            return;
        }
        if (!e->used) {
            slot = i;
            break;
        }
        if (e->age < oldest) {
            oldest = e->age;
            slot = i;
        }
    }

    vfs_dcache_entry_t old = {0};
    if (g_dcache[slot].used)
        old = g_dcache[slot];

    vfs_dcache_entry_t *e = &g_dcache[slot];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->mnt = dir->mnt;
    e->parent_ino = dir->ino;
    strncpy(e->name, name, MAX_NAME_LEN - 1);
    e->vn = vn;
    e->age = ++g_dcache_age;
    vnode_get(vn);
    spin_unlock(&g_dcache_lock);

    if (old.used && old.vn)
        vnode_put(old.vn);
}

void vfs_dcache_invalidate_all(void)
{
    vfs_dcache_entry_t old[VFS_DCACHE_MAX];

    spin_lock(&g_dcache_lock);
    memcpy(old, g_dcache, sizeof(old));
    memset(g_dcache, 0, sizeof(g_dcache));
    spin_unlock(&g_dcache_lock);

    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        if (old[i].used && old[i].vn)
            vnode_put(old[i].vn);
    }
}

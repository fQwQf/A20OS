#include "fs/vfs/dcache.h"
#include "core/lock.h"
#include "core/string.h"

#define VFS_DCACHE_MAX 512
#define VFS_DCACHE_HASH_BITS 7
#define VFS_DCACHE_HASH_SIZE (1U << VFS_DCACHE_HASH_BITS)
#define VFS_DCACHE_HASH_MASK (VFS_DCACHE_HASH_SIZE - 1)

typedef struct {
    int used;
    int hash_next;
    int hash_prev;
    mount_t *mnt;
    uint64_t parent_ino;
    char name[MAX_NAME_LEN];
    vnode_t *vn;
    uint64_t age;
} vfs_dcache_entry_t;

static spinlock_t g_dcache_lock = SPINLOCK_INIT;
static vfs_dcache_entry_t g_dcache[VFS_DCACHE_MAX];
static int g_dcache_hash[VFS_DCACHE_HASH_SIZE];
static uint64_t g_dcache_age;
static int g_dcache_free_list;
static int g_dcache_free_count;

static uint32_t dcache_hash_key(mount_t *mnt, uint64_t ino, const char *name)
{
    uint32_t h = (uint32_t)(uintptr_t)mnt ^ (uint32_t)ino;
    for (const char *p = name; *p; p++)
        h = h * 31 + (uint8_t)*p;
    return h & VFS_DCACHE_HASH_MASK;
}

static void dcache_init_freelist(void)
{
    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        g_dcache[i].hash_next = -1;
        g_dcache[i].hash_prev = -1;
        g_dcache[i].used = 0;
        g_dcache[i].age = 0;
    }
    for (int i = 0; i < (int)VFS_DCACHE_HASH_SIZE; i++)
        g_dcache_hash[i] = -1;
    g_dcache_free_list = 0;
    g_dcache_free_count = VFS_DCACHE_MAX;
}

static int dcache_alloc_slot(void)
{
    if (g_dcache_free_count == 0) {
        uint64_t oldest = (uint64_t)-1;
        int victim = -1;
        for (int i = 0; i < VFS_DCACHE_MAX; i++) {
            if (g_dcache[i].used && g_dcache[i].age < oldest) {
                oldest = g_dcache[i].age;
                victim = i;
            }
        }
        return victim;
    }
    int slot = g_dcache_free_list;
    if (slot < 0 || slot >= VFS_DCACHE_MAX) return -1;
    g_dcache_free_list++;
    g_dcache_free_count--;
    return slot;
}

static void dcache_unlink_hash(int slot)
{
    vfs_dcache_entry_t *e = &g_dcache[slot];
    if (e->hash_prev >= 0)
        g_dcache[e->hash_prev].hash_next = e->hash_next;
    else {
        uint32_t h = dcache_hash_key(e->mnt, e->parent_ino, e->name);
        g_dcache_hash[h] = e->hash_next;
    }
    if (e->hash_next >= 0)
        g_dcache[e->hash_next].hash_prev = e->hash_prev;
    e->hash_next = -1;
    e->hash_prev = -1;
}

static void dcache_link_hash(int slot, uint32_t h)
{
    g_dcache[slot].hash_next = g_dcache_hash[h];
    g_dcache[slot].hash_prev = -1;
    if (g_dcache_hash[h] >= 0)
        g_dcache[g_dcache_hash[h]].hash_prev = slot;
    g_dcache_hash[h] = slot;
}

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

    uint32_t h = dcache_hash_key(dir->mnt, dir->ino, name);

    uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0 && e->vn && vnode_ref_read(e->vn) > 0) {
            e->age = ++g_dcache_age;
            vnode_get(e->vn);
            vnode_t *vn = e->vn;
            spin_unlock_irqrestore(&g_dcache_lock, flags);
            return vn;
        }
    }
    spin_unlock_irqrestore(&g_dcache_lock, flags);
    return NULL;
}

void vfs_dcache_insert(vnode_t *dir, const char *name, vnode_t *vn)
{
    if (!vfs_dcache_enabled_for(dir) || !name || !*name || !vn)
        return;

    uint32_t h = dcache_hash_key(dir->mnt, dir->ino, name);

    uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0) {
            e->age = ++g_dcache_age;
            spin_unlock_irqrestore(&g_dcache_lock, flags);
            return;
        }
    }

    int slot = dcache_alloc_slot();
    if (slot < 0) {
        spin_unlock_irqrestore(&g_dcache_lock, flags);
        return;
    }

    vnode_t *old_vn = NULL;
    if (g_dcache[slot].used) {
        old_vn = g_dcache[slot].vn;
        dcache_unlink_hash(slot);
    }

    vfs_dcache_entry_t *e = &g_dcache[slot];
    memset(e, 0, sizeof(*e));
    e->used = 1;
    e->mnt = dir->mnt;
    e->parent_ino = dir->ino;
    strncpy(e->name, name, MAX_NAME_LEN - 1);
    e->vn = vn;
    e->age = ++g_dcache_age;
    e->hash_next = -1;
    e->hash_prev = -1;
    dcache_link_hash(slot, h);
    vnode_get(vn);
    spin_unlock_irqrestore(&g_dcache_lock, flags);

    if (old_vn)
        vnode_put(old_vn);
}

void vfs_dcache_invalidate(vnode_t *dir, const char *name)
{
    if (!dir || !name) return;
    uint32_t h = dcache_hash_key(dir->mnt, dir->ino, name);

    uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0) {
            vnode_t *vn = e->vn;
            dcache_unlink_hash(i);
            memset(e, 0, sizeof(*e));
            e->hash_next = -1;
            e->hash_prev = -1;
            spin_unlock_irqrestore(&g_dcache_lock, flags);
            if (vn) vnode_put(vn);
            return;
        }
    }
    spin_unlock_irqrestore(&g_dcache_lock, flags);
}

void vfs_dcache_invalidate_all(void)
{
    uint64_t flags = spin_lock_irqsave(&g_dcache_lock);
    vnode_t *to_put[VFS_DCACHE_MAX];
    int count = 0;
    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        if (g_dcache[i].used && g_dcache[i].vn)
            to_put[count++] = g_dcache[i].vn;
    }
    dcache_init_freelist();
    spin_unlock_irqrestore(&g_dcache_lock, flags);

    for (int i = 0; i < count; i++)
        vnode_put(to_put[i]);
}

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
    int free_next;
    int lru_next;
    int lru_prev;
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
static int g_dcache_lru_head;
static int g_dcache_lru_tail;
static int g_dcache_initialized;

static uint32_t dcache_hash_key(mount_t *mnt, uint64_t ino, const char *name)
{
    uint32_t h = (uint32_t)(uintptr_t)mnt ^ (uint32_t)ino;
    for (const char *p = name; *p; p++)
        h = h * 31 + (uint8_t)*p;
    return h & VFS_DCACHE_HASH_MASK;
}

static void dcache_init_locked(void)
{
    if (g_dcache_initialized)
        return;
    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        g_dcache[i].hash_next = -1;
        g_dcache[i].hash_prev = -1;
        g_dcache[i].free_next = i + 1;
        g_dcache[i].lru_next = -1;
        g_dcache[i].lru_prev = -1;
        g_dcache[i].used = 0;
        g_dcache[i].age = 0;
    }
    g_dcache[VFS_DCACHE_MAX - 1].free_next = -1;
    for (int i = 0; i < (int)VFS_DCACHE_HASH_SIZE; i++)
        g_dcache_hash[i] = -1;
    g_dcache_free_list = 0;
    g_dcache_free_count = VFS_DCACHE_MAX;
    g_dcache_lru_head = -1;
    g_dcache_lru_tail = -1;
    g_dcache_initialized = 1;
}

static void dcache_lru_remove(int slot)
{
    vfs_dcache_entry_t *e = &g_dcache[slot];
    if (e->lru_prev >= 0)
        g_dcache[e->lru_prev].lru_next = e->lru_next;
    else
        g_dcache_lru_head = e->lru_next;
    if (e->lru_next >= 0)
        g_dcache[e->lru_next].lru_prev = e->lru_prev;
    else
        g_dcache_lru_tail = e->lru_prev;
    e->lru_next = -1;
    e->lru_prev = -1;
}

static void dcache_lru_insert_front(int slot)
{
    vfs_dcache_entry_t *e = &g_dcache[slot];
    e->lru_prev = -1;
    e->lru_next = g_dcache_lru_head;
    if (g_dcache_lru_head >= 0)
        g_dcache[g_dcache_lru_head].lru_prev = slot;
    else
        g_dcache_lru_tail = slot;
    g_dcache_lru_head = slot;
}

static void dcache_lru_touch(int slot)
{
    if (g_dcache_lru_head == slot)
        return;
    dcache_lru_remove(slot);
    dcache_lru_insert_front(slot);
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

static int dcache_alloc_slot(vnode_t **old_vn)
{
    if (g_dcache_free_count > 0) {
        int slot = g_dcache_free_list;
        if (slot < 0 || slot >= VFS_DCACHE_MAX)
            return -1;
        g_dcache_free_list = g_dcache[slot].free_next;
        g_dcache[slot].free_next = -1;
        g_dcache_free_count--;
        return slot;
    }

    int slot = g_dcache_lru_tail;
    if (slot < 0)
        return -1;
    *old_vn = g_dcache[slot].vn;
    dcache_unlink_hash(slot);
    dcache_lru_remove(slot);
    return slot;
}

static void dcache_free_slot(int slot)
{
    vfs_dcache_entry_t *e = &g_dcache[slot];
    dcache_unlink_hash(slot);
    dcache_lru_remove(slot);
    memset(e, 0, sizeof(*e));
    e->hash_next = -1;
    e->hash_prev = -1;
    e->lru_next = -1;
    e->lru_prev = -1;
    e->free_next = g_dcache_free_list;
    g_dcache_free_list = slot;
    g_dcache_free_count++;
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
    dcache_init_locked();
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0 && e->vn && vnode_ref_read(e->vn) > 0) {
            e->age = ++g_dcache_age;
            dcache_lru_touch(i);
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
    dcache_init_locked();
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0) {
            e->age = ++g_dcache_age;
            dcache_lru_touch(i);
            spin_unlock_irqrestore(&g_dcache_lock, flags);
            return;
        }
    }

    vnode_t *old_vn = NULL;
    int slot = dcache_alloc_slot(&old_vn);
    if (slot < 0) {
        spin_unlock_irqrestore(&g_dcache_lock, flags);
        return;
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
    e->free_next = -1;
    e->lru_next = -1;
    e->lru_prev = -1;
    dcache_link_hash(slot, h);
    dcache_lru_insert_front(slot);
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
    dcache_init_locked();
    for (int i = g_dcache_hash[h]; i >= 0; i = g_dcache[i].hash_next) {
        vfs_dcache_entry_t *e = &g_dcache[i];
        if (e->used && e->mnt == dir->mnt && e->parent_ino == dir->ino &&
            strcmp(e->name, name) == 0) {
            vnode_t *vn = e->vn;
            dcache_free_slot(i);
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
    dcache_init_locked();
    vnode_t *to_put[VFS_DCACHE_MAX];
    int count = 0;
    for (int i = 0; i < VFS_DCACHE_MAX; i++) {
        if (g_dcache[i].used && g_dcache[i].vn)
            to_put[count++] = g_dcache[i].vn;
    }
    g_dcache_initialized = 0;
    dcache_init_locked();
    spin_unlock_irqrestore(&g_dcache_lock, flags);

    for (int i = 0; i < count; i++)
        vnode_put(to_put[i]);
}

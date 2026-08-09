#include "fs/file_handle.h"

#include "core/lock.h"
#include "core/string.h"
#include "fs/vfs.h"
#include "mm/slab.h"

/*
 * Opaque file handle registry.
 *
 * A small fixed table guards against handle flooding.  Each entry holds a
 * referenced vnode and a generation cookie; the 64-bit handle value packs the
 * table index (low bits) and generation (high bits) so stale handles fail
 * fast even after a slot is reused.
 */

#define FILE_HANDLE_TABLE_SIZE 64
#define FILE_HANDLE_GEN_MASK   0xffffffff00000000ULL
#define FILE_HANDLE_IDX_MASK   0x00000000ffffffffULL

typedef struct file_handle_entry {
    int used;
    struct vnode *vn;      /* referenced */
    uint32_t generation;
} file_handle_entry_t;

static file_handle_entry_t g_handles[FILE_HANDLE_TABLE_SIZE];
static spinlock_t g_handle_lock = SPINLOCK_INIT;
static uint32_t g_handle_next_gen = 1;

uint64_t file_handle_mint(struct vnode *vn)
{
    if (!vn)
        return 0;

    uint64_t flags = spin_lock_irqsave(&g_handle_lock);
    for (int i = 0; i < FILE_HANDLE_TABLE_SIZE; i++) {
        file_handle_entry_t *e = &g_handles[i];
        if (!e->used) {
            vnode_get(vn);
            e->used = 1;
            e->vn = vn;
            e->generation = g_handle_next_gen++;
            if (g_handle_next_gen == 0)
                g_handle_next_gen = 1;
            uint64_t handle =
                ((uint64_t)e->generation << 32) | (uint32_t)i;
            spin_unlock_irqrestore(&g_handle_lock, flags);
            return handle;
        }
    }
    spin_unlock_irqrestore(&g_handle_lock, flags);
    return 0;
}

struct vnode *file_handle_get(uint64_t handle)
{
    uint32_t idx = (uint32_t)(handle & FILE_HANDLE_IDX_MASK);
    uint32_t gen = (uint32_t)((handle & FILE_HANDLE_GEN_MASK) >> 32);
    if (idx >= FILE_HANDLE_TABLE_SIZE || gen == 0)
        return NULL;

    uint64_t flags = spin_lock_irqsave(&g_handle_lock);
    file_handle_entry_t *e = &g_handles[idx];
    struct vnode *vn = NULL;
    if (e->used && e->generation == gen && e->vn) {
        vnode_get(e->vn);
        vn = e->vn;
    }
    spin_unlock_irqrestore(&g_handle_lock, flags);
    return vn;
}

void file_handle_drop(uint64_t handle)
{
    uint32_t idx = (uint32_t)(handle & FILE_HANDLE_IDX_MASK);
    uint32_t gen = (uint32_t)((handle & FILE_HANDLE_GEN_MASK) >> 32);
    if (idx >= FILE_HANDLE_TABLE_SIZE || gen == 0)
        return;

    uint64_t flags = spin_lock_irqsave(&g_handle_lock);
    file_handle_entry_t *e = &g_handles[idx];
    if (e->used && e->generation == gen) {
        e->used = 0;
        e->generation = 0;
        struct vnode *vn = e->vn;
        e->vn = NULL;
        spin_unlock_irqrestore(&g_handle_lock, flags);
        if (vn)
            vnode_put(vn);
        return;
    }
    spin_unlock_irqrestore(&g_handle_lock, flags);
}

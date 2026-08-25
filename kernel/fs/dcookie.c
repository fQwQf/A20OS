#include "fs/dcookie.h"

#include "core/lock.h"
#include "core/string.h"
#include "fs/vfs.h"
#include "mm/slab.h"

#define DCOOKIE_MAX_SLOTS 64

typedef struct {
    int in_use;
    int pinned;
    int dead;
    uint64_t cookie;
    char *path;
    vnode_t *vn;
} dcookie_slot_t;

static spinlock_t g_dcookie_lock;
static dcookie_slot_t g_dcookie_slots[DCOOKIE_MAX_SLOTS];
static uint32_t g_dcookie_gen = 1;

static void dcookie_free_locked(dcookie_slot_t *s)
{
    kfree(s->path);
    s->path = NULL;
    s->in_use = 0;
    s->dead = 0;
    s->cookie = 0;
}

uint64_t dcookie_register(vnode_t *vn, const char *path)
{
    if (!path)
        return 0;

    char *copy = kmalloc(strlen(path) + 1);
    if (!copy)
        return 0;
    strcpy(copy, path);

    uint64_t flags = spin_lock_irqsave(&g_dcookie_lock);
    for (int i = 0; i < DCOOKIE_MAX_SLOTS; i++) {
        dcookie_slot_t *s = &g_dcookie_slots[i];
        if (s->in_use)
            continue;
        s->in_use = 1;
        s->pinned = 0;
        s->dead = 0;
        s->path = copy;
        s->vn = vn;
        if (vn)
            vnode_get(vn);
        s->cookie = ((uint64_t)g_dcookie_gen++ << 16) | (uint64_t)(i + 1);
        spin_unlock_irqrestore(&g_dcookie_lock, flags);
        return s->cookie;
    }
    spin_unlock_irqrestore(&g_dcookie_lock, flags);
    kfree(copy);
    return 0;
}

void dcookie_revoke(uint64_t cookie)
{
    uint64_t flags = spin_lock_irqsave(&g_dcookie_lock);
    for (int i = 0; i < DCOOKIE_MAX_SLOTS; i++) {
        dcookie_slot_t *s = &g_dcookie_slots[i];
        if (!s->in_use || s->dead || s->cookie != cookie)
            continue;
        if (s->pinned > 0) {
            /* A lookup still reads the path; defer the free to release(). */
            s->dead = 1;
        } else {
            vnode_t *vn = s->vn;
            s->vn = NULL;
            dcookie_free_locked(s);
            spin_unlock_irqrestore(&g_dcookie_lock, flags);
            if (vn)
                vnode_put(vn);
            return;
        }
        break;
    }
    spin_unlock_irqrestore(&g_dcookie_lock, flags);
}

int dcookie_resolve(uint64_t cookie, char **path_out)
{
    if (!path_out || !cookie)
        return -1;
    uint64_t flags = spin_lock_irqsave(&g_dcookie_lock);
    for (int i = 0; i < DCOOKIE_MAX_SLOTS; i++) {
        dcookie_slot_t *s = &g_dcookie_slots[i];
        if (!s->in_use || s->dead || s->cookie != cookie)
            continue;
        s->pinned++;
        *path_out = s->path;
        spin_unlock_irqrestore(&g_dcookie_lock, flags);
        return i;
    }
    spin_unlock_irqrestore(&g_dcookie_lock, flags);
    return -1;
}

void dcookie_release(int idx)
{
    if (idx < 0 || idx >= DCOOKIE_MAX_SLOTS)
        return;
    uint64_t flags = spin_lock_irqsave(&g_dcookie_lock);
    dcookie_slot_t *s = &g_dcookie_slots[idx];
    if (!s->in_use || s->pinned <= 0) {
        spin_unlock_irqrestore(&g_dcookie_lock, flags);
        return;
    }
    s->pinned--;
    if (s->pinned == 0 && s->dead) {
        vnode_t *vn = s->vn;
        s->vn = NULL;
        dcookie_free_locked(s);
        spin_unlock_irqrestore(&g_dcookie_lock, flags);
        if (vn)
            vnode_put(vn);
        return;
    }
    spin_unlock_irqrestore(&g_dcookie_lock, flags);
}

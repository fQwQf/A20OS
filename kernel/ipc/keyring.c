#include "ipc/keyring.h"

#include "core/consts.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "mm/slab.h"
#include "proc/proc.h"
#include <limits.h>

/*
 * Kernel keyring subsystem.
 *
 * Object model:
 *   - Every key/keyring is a task_key_object_t carrying a serial number.  The
 *     global serial map is a singly-linked list guarded by g_keyring_lock.
 *   - A keyring holds a dynamic array of referenced child objects.
 *   - task_t->session_keyring owns one reference (set by JOIN_SESSION_KEYRING
 *     or lazily by add_key); it is shared with children at fork and released
 *     at task teardown.
 *   - Per-uid user keyrings are cached in g_user_rings.
 */

typedef enum {
    TASK_KEY_OBJECT_KEY = 1,
    TASK_KEY_OBJECT_KEYRING = 2,
} task_key_object_type_t;

typedef struct task_key_object {
    struct task_key_object *next;   /* global serial map list */
    task_key_object_type_t type;
    key_serial_t serial;
    int ref_count;
    int64_t uid;
    int64_t gid;
    uint32_t perm;
    uint64_t expiry_ns;
    char *description;
    /* KEY */
    char *key_type;
    void *payload;
    size_t payload_len;
    /* KEYRING */
    struct task_key_object **items;
    size_t item_count;
    size_t item_capacity;
    uint32_t graph_visit;
    int persistent_user;
    uint64_t owner_uid;
} task_key_object_t;

#define KEYRING_ITEM_INIT_CAP 4
#define KEYRING_MAX_OBJECTS   4096

static spinlock_t g_keyring_lock = SPINLOCK_INIT;
static task_key_object_t *g_serial_map;
static int g_next_serial = 1;
static int g_object_count;
static uint32_t g_graph_generation;
static task_key_object_t *g_graph_stack[KEYRING_MAX_OBJECTS];

static uint64_t keyring_now_ns(void)
{
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    return ts[0] * 1000000000ULL + ts[1];
}

static task_key_object_t *keyring_alloc_object(task_key_object_type_t type,
                                               const char *desc,
                                               int64_t uid, int64_t gid)
{
    task_key_object_t *o = kcalloc(1, sizeof(*o));
    if (!o)
        return NULL;
    o->type = type;
    o->uid = uid;
    o->gid = gid;
    o->perm = KEY_POS_ALL | KEY_USR_ALL;
    if (desc)
        o->description = strdup(desc);
    if (!o->description) {
        kfree(o);
        return NULL;
    }
    return o;
}

static void keyring_destroy_locked(task_key_object_t *o);
static void keyring_put_locked(task_key_object_t *o);
static void keyring_unlink_from_serial_map_locked(task_key_object_t *o);
static task_key_object_t *keyring_lookup_locked(key_serial_t serial);

static void keyring_destroy_locked(task_key_object_t *o)
{
    /* `next` is free for an intrusive worklist after map removal. Reclaim
     * nested graphs iteratively so even a maximum-depth DAG cannot exhaust a
     * kernel stack from task teardown under irqsave. */
    task_key_object_t *pending = o;
    while (pending) {
        task_key_object_t *current = pending;
        pending = current->next;
        current->next = NULL;
        if (current->type == TASK_KEY_OBJECT_KEYRING) {
            for (size_t i = 0; i < current->item_count; i++) {
                task_key_object_t *child = current->items[i];
                if (!child || child->ref_count <= 0)
                    continue;
                if (--child->ref_count == 0) {
                    keyring_unlink_from_serial_map_locked(child);
                    child->next = pending;
                    pending = child;
                }
            }
            kfree(current->items);
        } else {
            kfree(current->payload);
            kfree(current->key_type);
        }
        kfree(current->description);
        kfree(current);
    }
}

static void keyring_unlink_from_serial_map_locked(task_key_object_t *o)
{
    task_key_object_t **pp = &g_serial_map;
    while (*pp) {
        if (*pp == o) {
            *pp = o->next;
            o->next = NULL;
            g_object_count--;
            return;
        }
        pp = &(*pp)->next;
    }
}

static void keyring_put_locked(task_key_object_t *o)
{
    if (!o)
        return;
    if (o->ref_count <= 0)
        return;
    if (--o->ref_count > 0)
        return;
    keyring_unlink_from_serial_map_locked(o);
    keyring_destroy_locked(o);
}

/* The global lock serializes this bounded iterative traversal. Marking nodes
 * avoids recursion and guarantees a link cannot create a reference cycle. */
static int keyring_graph_reaches_locked(task_key_object_t *start,
                                        task_key_object_t *target)
{
    if (start == target)
        return 1;
    uint32_t generation = ++g_graph_generation;
    if (generation == 0) {
        for (task_key_object_t *o = g_serial_map; o; o = o->next)
            o->graph_visit = 0;
        generation = ++g_graph_generation;
    }

    size_t count = 1;
    start->graph_visit = generation;
    g_graph_stack[0] = start;
    while (count > 0) {
        task_key_object_t *ring = g_graph_stack[--count];
        for (size_t i = 0; i < ring->item_count; i++) {
            task_key_object_t *child = ring->items[i];
            if (!child || child->type != TASK_KEY_OBJECT_KEYRING)
                continue;
            if (child == target)
                return 1;
            if (child->graph_visit == generation)
                continue;
            if (count == KEYRING_MAX_OBJECTS)
                return 1; /* fail closed if map accounting is corrupt */
            child->graph_visit = generation;
            g_graph_stack[count++] = child;
        }
    }
    return 0;
}

static int keyring_alloc_serial_locked(void)
{
    if (g_object_count >= KEYRING_MAX_OBJECTS)
        return 0;
    int attempts = g_object_count + 1;
    while (attempts-- > 0) {
        int serial = g_next_serial;
        g_next_serial = serial == INT_MAX ? 1 : serial + 1;
        if (serial > 0 && !keyring_lookup_locked(serial))
            return serial;
    }
    return 0;
}

static int keyring_link_locked(task_key_object_t *ring,
                               task_key_object_t *o)
{
    if (!ring || !o || ring->type != TASK_KEY_OBJECT_KEYRING)
        return -EINVAL;
    if (ring->serial == o->serial)
        return -EINVAL;
    for (size_t i = 0; i < ring->item_count; i++) {
        if (ring->items[i] == o)
            return 0; /* already linked */
    }
    if (o->type == TASK_KEY_OBJECT_KEYRING &&
        keyring_graph_reaches_locked(o, ring))
        return -EDEADLK;
    if (o->ref_count < 0)
        return -EINVAL;
    if (o->ref_count == INT_MAX)
        return -ENOSPC;
    if (ring->item_count == ring->item_capacity) {
        size_t cap = ring->item_capacity ? ring->item_capacity
                                         : KEYRING_ITEM_INIT_CAP;
        while (cap <= ring->item_count)
            cap <<= 1;
        task_key_object_t **ni =
            kmalloc(cap * sizeof(*ni));
        if (!ni)
            return -ENOMEM;
        if (ring->items) {
            memcpy(ni, ring->items,
                   ring->item_count * sizeof(*ni));
            kfree(ring->items);
        }
        ring->items = ni;
        ring->item_capacity = cap;
    }
    ring->items[ring->item_count++] = o;
    o->ref_count++;
    return 0;
}

static int keyring_unlink_locked(task_key_object_t *ring,
                                 task_key_object_t *o)
{
    if (!ring || !o || ring->type != TASK_KEY_OBJECT_KEYRING)
        return -EINVAL;
    for (size_t i = 0; i < ring->item_count; i++) {
        if (ring->items[i] != o)
            continue;
        memmove(&ring->items[i], &ring->items[i + 1],
                (ring->item_count - i - 1) * sizeof(*ring->items));
        ring->item_count--;
        keyring_put_locked(o);
        return 0;
    }
    return -ENOENT;
}

static task_key_object_t *keyring_lookup_locked(key_serial_t serial)
{
    for (task_key_object_t *o = g_serial_map; o; o = o->next)
        if (o->serial == serial)
            return o;
    return NULL;
}

static task_key_object_t *keyring_user_ring_locked(int64_t uid, int64_t gid,
                                                   int create)
{
    for (task_key_object_t *o = g_serial_map; o; o = o->next) {
        if (o->type == TASK_KEY_OBJECT_KEYRING && o->persistent_user &&
            o->owner_uid == (uint64_t)uid)
            return o;
    }
    if (!create || uid < 0)
        return NULL;
    task_key_object_t *ring =
        keyring_alloc_object(TASK_KEY_OBJECT_KEYRING, "_uid", uid, gid);
    if (!ring)
        return NULL;
    ring->serial = keyring_alloc_serial_locked();
    if (ring->serial == 0) {
        keyring_destroy_locked(ring);
        return NULL;
    }
    ring->ref_count = 1; /* persistent */
    ring->persistent_user = 1;
    ring->owner_uid = (uint64_t)uid;
    ring->next = g_serial_map;
    g_serial_map = ring;
    g_object_count++;
    return ring;
}

static task_key_object_t *keyring_session_ring_locked(task_t *t, int create)
{
    if (!t)
        return NULL;
    if (t->session_keyring)
        return (task_key_object_t *)t->session_keyring;
    if (!create)
        return NULL;
    task_key_object_t *ring =
        keyring_alloc_object(TASK_KEY_OBJECT_KEYRING, "_ses", t->cred.uid,
                             t->cred.gid);
    if (!ring)
        return NULL;
    ring->serial = keyring_alloc_serial_locked();
    if (ring->serial == 0) {
        keyring_destroy_locked(ring);
        return NULL;
    }
    ring->ref_count = 1;
    ring->next = g_serial_map;
    g_serial_map = ring;
    g_object_count++;
    t->session_keyring = ring;
    return ring;
}

static task_key_object_t *keyring_resolve_locked(task_t *t, key_serial_t id,
                                                 int create_keyring)
{
    switch (id) {
    case KEY_SPEC_THREAD_KEYRING:
    case KEY_SPEC_PROCESS_KEYRING:
    case KEY_SPEC_SESSION_KEYRING:
        return keyring_session_ring_locked(t, create_keyring);
    case KEY_SPEC_USER_KEYRING:
    case KEY_SPEC_USER_SESSION_KEYRING:
        return keyring_user_ring_locked(t->cred.uid, t->cred.gid,
                                        create_keyring);
    default:
        if (id <= 0)
            return NULL;
        {
            task_key_object_t *o = keyring_lookup_locked(id);
            return o && o->type == TASK_KEY_OBJECT_KEYRING ? o : NULL;
        }
    }
}

static task_key_object_t *keyring_object_resolve_locked(task_t *t,
                                                        key_serial_t id,
                                                        int create_keyring)
{
    if (id < 0)
        return (task_key_object_t *)keyring_resolve_locked(t, id,
                                                           create_keyring);
    return keyring_lookup_locked(id);
}

static int keyring_is_expired(task_key_object_t *o, uint64_t now_ns)
{
    return o && o->expiry_ns != 0 && now_ns >= o->expiry_ns;
}

/* Permission needs, in Linux's key_need_perm order. */
enum {
    KEY_NEED_VIEW = 1 << 0,
    KEY_NEED_READ = 1 << 1,
    KEY_NEED_WRITE = 1 << 2,
    KEY_NEED_SEARCH = 1 << 3,
    KEY_NEED_LINK = 1 << 4,
    KEY_NEED_SETATTR = 1 << 5,
};

/* The bits an operation needs, expressed at KEY_POS_* positions; group and
 * other classes shift the same pattern down by 16 and 24 bits. */
static uint32_t keyring_need_pos_bits(int need)
{
    uint32_t bits = 0;
    if (need & (KEY_NEED_VIEW | KEY_NEED_READ | KEY_NEED_WRITE |
                KEY_NEED_SEARCH | KEY_NEED_LINK))
        bits |= KEY_POS_VIEW;
    if (need & KEY_NEED_READ)
        bits |= KEY_POS_READ;
    if (need & KEY_NEED_WRITE)
        bits |= KEY_POS_WRITE;
    if (need & KEY_NEED_SEARCH)
        bits |= KEY_POS_SEARCH;
    if (need & KEY_NEED_LINK)
        bits |= KEY_POS_LINK;
    if (need & KEY_NEED_SETATTR)
        bits |= KEY_POS_SETATTR;
    return bits;
}

/* Class is the effective owner/group/other triple.  Supplementary groups are
 * not part of this kernel's credential view, so only egid can match. */
static int keyring_permitted(task_t *t, task_key_object_t *o, int need)
{
    if (!t || !o)
        return 0;
    uint32_t class_mask;
    int shift;
    if (o->uid == t->cred.euid) {
        class_mask = KEY_POS_ALL;
        shift = 0;
    } else if (o->gid == t->cred.egid) {
        class_mask = KEY_GRP_ALL;
        shift = 16;
    } else {
        class_mask = KEY_OTH_ALL;
        shift = 24;
    }
    uint32_t needed = keyring_need_pos_bits(need) >> shift;
    return (o->perm & class_mask & needed) == needed;
}

static task_key_object_t *keyring_find_locked(task_t *t,
                                              task_key_object_t *ring,
                                              const char *type,
                                              const char *desc,
                                              int direct_only, int depth,
                                              uint64_t now_ns)
{
    if (!ring || !type || !desc || depth < 0)
        return NULL;
    /* Searching a ring requires SEARCH on that ring.  add_key's duplicate
     * probe is authorised by the ring's WRITE permission instead. */
    if (!direct_only && !keyring_permitted(t, ring, KEY_NEED_SEARCH))
        return NULL;
    for (size_t i = 0; i < ring->item_count; i++) {
        task_key_object_t *o = ring->items[i];
        if (!o || keyring_is_expired(o, now_ns))
            continue;
        if (o->type == TASK_KEY_OBJECT_KEY) {
            if (strcmp(o->key_type, type) == 0 &&
                strcmp(o->description, desc) == 0 &&
                (direct_only || keyring_permitted(t, o, KEY_NEED_SEARCH)))
                return o;
            continue;
        }
        if (!direct_only && depth > 0) {
            task_key_object_t *nested =
                keyring_find_locked(t, o, type, desc, 0, depth - 1, now_ns);
            if (nested)
                return nested;
        }
    }
    return NULL;
}

/* ---- public API ---- */

key_serial_t keyring_add_key(const char *type, const char *desc,
                             const void *payload, size_t plen,
                             key_serial_t ringid)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;

    task_key_object_t *new_key =
        keyring_alloc_object(TASK_KEY_OBJECT_KEY, desc, t->cred.euid,
                             t->cred.egid);
    if (!new_key)
        return -ENOMEM;
    new_key->key_type = strdup(type);
    if (!new_key->key_type) {
        keyring_destroy_locked(new_key);
        return -ENOMEM;
    }
    if (plen > 0) {
        new_key->payload = kmalloc(plen);
        if (!new_key->payload) {
            keyring_destroy_locked(new_key);
            return -ENOMEM;
        }
        memcpy(new_key->payload, payload, plen);
        new_key->payload_len = plen;
    }

    uint64_t now = keyring_now_ns();
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);

    task_key_object_t *ring =
        keyring_resolve_locked(t, ringid, 1);
    if (!ring) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        keyring_destroy_locked(new_key);
        return -ENOKEY;
    }

    if (!keyring_permitted(t, ring, KEY_NEED_WRITE)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        keyring_destroy_locked(new_key);
        return -EACCES;
    }

    task_key_object_t *existing =
        keyring_find_locked(t, ring, type, desc, 1, 0, now);
    if (existing) {
        kfree(existing->payload);
        existing->payload = new_key->payload;
        existing->payload_len = new_key->payload_len;
        existing->uid = t->cred.euid;
        existing->gid = t->cred.egid;
        existing->expiry_ns = 0;
        key_serial_t serial = existing->serial;
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        kfree(new_key->key_type);
        kfree(new_key->description);
        kfree(new_key);
        return serial;
    }

    new_key->serial = keyring_alloc_serial_locked();
    if (new_key->serial == 0) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        keyring_destroy_locked(new_key);
        return -ENOSPC;
    }
    /* ref_count starts at 0; keyring_link_locked() takes the single owning
     * reference on success.  The serial-map membership does not hold a
     * reference. */
    new_key->next = g_serial_map;
    g_serial_map = new_key;
    g_object_count++;

    int ret = keyring_link_locked(ring, new_key);
    if (ret < 0) {
        keyring_unlink_from_serial_map_locked(new_key);
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        keyring_destroy_locked(new_key);
        return ret;
    }
    key_serial_t serial = new_key->serial;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return serial;
}

key_serial_t keyring_request_key(const char *type, const char *desc,
                                 key_serial_t dest_ringid)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;

    uint64_t now = keyring_now_ns();
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);

    task_key_object_t *session =
        keyring_resolve_locked(t, KEY_SPEC_SESSION_KEYRING, 0);
    task_key_object_t *key =
        session ? keyring_find_locked(t, session, type, desc, 0,
                                      KEYRING_SEARCH_DEPTH, now)
                : NULL;
    if (!key) {
        task_key_object_t *user =
            keyring_resolve_locked(t, KEY_SPEC_USER_KEYRING, 0);
        if (user)
            key = keyring_find_locked(t, user, type, desc, 0, 1, now);
    }
    if (!key) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }

    if (dest_ringid != 0) {
        task_key_object_t *dest =
            keyring_resolve_locked(t, dest_ringid, 1);
        if (dest)
            (void)keyring_link_locked(dest, key);
    }
    key_serial_t serial = key->serial;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return serial;
}

key_serial_t keyring_get_keyring_id(key_serial_t id, int create)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *ring = keyring_resolve_locked(t, id, create);
    key_serial_t ret = ring ? ring->serial : -ENOKEY;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return ret;
}

key_serial_t keyring_join_session(const char *name)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;

    task_key_object_t *new_ring =
        keyring_alloc_object(TASK_KEY_OBJECT_KEYRING,
                             name ? name : "_ses", t->cred.uid, t->cred.gid);
    if (!new_ring)
        return -ENOMEM;

    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    new_ring->serial = keyring_alloc_serial_locked();
    if (new_ring->serial == 0) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        keyring_destroy_locked(new_ring);
        return -ENOSPC;
    }
    new_ring->ref_count = 1;
    new_ring->next = g_serial_map;
    g_serial_map = new_ring;
    g_object_count++;

    task_key_object_t *old = (task_key_object_t *)t->session_keyring;
    t->session_keyring = new_ring;
    if (old) {
        /* old had ref_count 1 held by the task slot */
        keyring_put_locked(old);
    }
    key_serial_t serial = new_ring->serial;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return serial;
}

int keyring_chown(key_serial_t id, int uid, int gid)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 0);
    if (!o) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (!proc_has_cap(t, CAP_SYS_ADMIN) && o->uid != t->cred.euid) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EPERM;
    }
    if (uid != -1)
        o->uid = uid;
    if (gid != -1)
        o->gid = gid;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return 0;
}

int keyring_setperm(key_serial_t id, uint32_t perm)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 0);
    if (!o) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (!proc_has_cap(t, CAP_SYS_ADMIN) && o->uid != t->cred.euid) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EPERM;
    }
    o->perm = perm;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return 0;
}

int keyring_set_timeout(key_serial_t id, unsigned long secs)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 0);
    if (!o) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (!proc_has_cap(t, CAP_SYS_ADMIN) && o->uid != t->cred.euid) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EPERM;
    }
    if (secs == 0) {
        o->expiry_ns = 0;
    } else {
        const uint64_t scale = 1000000000ULL;
        uint64_t now = keyring_now_ns();
        const uint64_t max_ns = ~(uint64_t)0;
        uint64_t delta = (uint64_t)secs > max_ns / scale
            ? max_ns : (uint64_t)secs * scale;
        o->expiry_ns = delta > max_ns - now ? max_ns : now + delta;
    }
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return 0;
}

int keyring_link(key_serial_t id, key_serial_t ringid)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 1);
    task_key_object_t *ring = keyring_resolve_locked(t, ringid, 1);
    if (!o || !ring) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (!keyring_permitted(t, o, KEY_NEED_LINK) ||
        !keyring_permitted(t, ring, KEY_NEED_WRITE)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EACCES;
    }
    int ret = keyring_link_locked(ring, o);
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return ret;
}

int keyring_unlink(key_serial_t id, key_serial_t ringid)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 1);
    task_key_object_t *ring = keyring_resolve_locked(t, ringid, 1);
    if (!o || !ring) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (!keyring_permitted(t, ring, KEY_NEED_WRITE)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EACCES;
    }
    int ret = keyring_unlink_locked(ring, o);
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return ret;
}

key_serial_t keyring_search(key_serial_t ringid, const char *type,
                            const char *desc)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    uint64_t now = keyring_now_ns();
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *ring = keyring_resolve_locked(t, ringid, 0);
    task_key_object_t *key =
        ring ? keyring_find_locked(t, ring, type, desc, 0,
                                   KEYRING_SEARCH_DEPTH, now)
             : NULL;
    key_serial_t ret = key ? key->serial : -ENOKEY;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return ret;
}

long keyring_describe(key_serial_t id, char *buf, size_t bufsz)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 0);
    if (!o) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (!keyring_permitted(t, o, KEY_NEED_VIEW)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EACCES;
    }
    const char *type_name =
        o->type == TASK_KEY_OBJECT_KEY ? o->key_type : "keyring";
    /* "type;uid;gid;%08x;desc" */
    int len = strlen(type_name) + 1 + 11 + 1 + 11 + 1 + 8 + 1 +
              (o->description ? (int)strlen(o->description) : 0);
    if (buf && bufsz > 0) {
        int n = snprintf(buf, bufsz, "%s;%lld;%lld;%08x;%s", type_name,
                         (long long)o->uid, (long long)o->gid, o->perm,
                         o->description ? o->description : "");
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return n < 0 ? -EINVAL : (long)n;
    }
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return len;
}

long keyring_read(key_serial_t id, void *buf, size_t bufsz)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    uint64_t now = keyring_now_ns();
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 0);
    if (!o || keyring_is_expired(o, now)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return o ? -EKEYEXPIRED : -ENOKEY;
    }
    if (!keyring_permitted(t, o, KEY_NEED_READ)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EACCES;
    }

    long ret;
    if (o->type == TASK_KEY_OBJECT_KEY) {
        size_t plen = o->payload_len;
        if (buf && bufsz >= plen && plen > 0)
            memcpy(buf, o->payload, plen);
        ret = (long)plen;
    } else {
        size_t n = o->item_count * sizeof(key_serial_t);
        if (buf && bufsz >= n) {
            key_serial_t *serials = buf;
            for (size_t i = 0; i < o->item_count; i++)
                serials[i] = o->items[i]->serial;
        }
        ret = (long)n;
    }
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return ret;
}

int keyring_clear(key_serial_t id)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *ring = keyring_resolve_locked(t, id, 0);
    if (!ring) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (!keyring_permitted(t, ring, KEY_NEED_WRITE)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EACCES;
    }
    while (ring->item_count > 0) {
        task_key_object_t *o = ring->items[0];
        int ret = keyring_unlink_locked(ring, o);
        if (ret < 0) {
            spin_unlock_irqrestore(&g_keyring_lock, flags);
            return ret;
        }
    }
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return 0;
}

static int keyring_ring_contains(task_key_object_t *ring,
                                 task_key_object_t *o)
{
    for (size_t i = 0; i < ring->item_count; i++)
        if (ring->items[i] == o)
            return 1;
    return 0;
}

int keyring_invalidate(key_serial_t id)
{
    task_t *t = proc_current();
    if (!t)
        return -ESRCH;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *o = keyring_object_resolve_locked(t, id, 0);
    if (!o) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -ENOKEY;
    }
    if (o->type != TASK_KEY_OBJECT_KEY) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EINVAL;
    }
    if (!keyring_permitted(t, o, KEY_NEED_WRITE)) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return -EPERM;
    }
    /* Expire before detaching so concurrent lookups stop matching; the final
     * unlink drops the last ring reference and reclaims the serial entry. */
    o->expiry_ns = 1;
    for (;;) {
        task_key_object_t *found = NULL;
        for (task_key_object_t *cursor = g_serial_map; cursor;
             cursor = cursor->next) {
            if (cursor->type == TASK_KEY_OBJECT_KEYRING && cursor != o &&
                keyring_ring_contains(cursor, o)) {
                found = cursor;
                break;
            }
        }
        if (!found)
            break;
        if (keyring_unlink_locked(found, o) < 0)
            break;
    }
    spin_unlock_irqrestore(&g_keyring_lock, flags);
    return 0;
}

void keyring_inherit(struct task_t *child, struct task_t *parent)
{
    if (!child || !parent || !parent->session_keyring)
        return;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *ring = (task_key_object_t *)parent->session_keyring;
    if (ring->ref_count <= 0 || ring->ref_count == INT_MAX) {
        spin_unlock_irqrestore(&g_keyring_lock, flags);
        return;
    }
    ring->ref_count++;
    child->session_keyring = ring;
    spin_unlock_irqrestore(&g_keyring_lock, flags);
}

void keyring_release_task(struct task_t *t)
{
    if (!t || !t->session_keyring)
        return;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *ring = (task_key_object_t *)t->session_keyring;
    t->session_keyring = NULL;
    keyring_put_locked(ring);
    spin_unlock_irqrestore(&g_keyring_lock, flags);
}

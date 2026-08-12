#include "ipc/keyring.h"

#include "core/consts.h"
#include "core/errno.h"
#include "core/lock.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/timekeeping.h"
#include "mm/slab.h"
#include "proc/proc.h"

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
    int persistent_user;
    uint64_t owner_uid;
} task_key_object_t;

#define KEYRING_ITEM_INIT_CAP 4
#define KEYRING_MAX_OBJECTS   4096

static spinlock_t g_keyring_lock = SPINLOCK_INIT;
static task_key_object_t *g_serial_map;
static int g_next_serial = 1;
static int g_object_count;

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

static void keyring_destroy_locked(task_key_object_t *o)
{
    if (!o)
        return;
    if (o->type == TASK_KEY_OBJECT_KEYRING) {
        for (size_t i = 0; i < o->item_count; i++)
            if (o->items[i])
                keyring_put_locked(o->items[i]);
        kfree(o->items);
    } else {
        kfree(o->payload);
        kfree(o->key_type);
    }
    kfree(o->description);
    kfree(o);
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
    if (--o->ref_count > 0)
        return;
    keyring_unlink_from_serial_map_locked(o);
    keyring_destroy_locked(o);
}

static int keyring_alloc_serial_locked(void)
{
    while (g_object_count < KEYRING_MAX_OBJECTS) {
        int serial = g_next_serial++;
        if (serial <= 0)
            g_next_serial = 1;
        else
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
        o->ref_count--;
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

static task_key_object_t *keyring_find_locked(task_key_object_t *ring,
                                              const char *type,
                                              const char *desc,
                                              int direct_only, int depth,
                                              uint64_t now_ns)
{
    if (!ring || !type || !desc || depth < 0)
        return NULL;
    for (size_t i = 0; i < ring->item_count; i++) {
        task_key_object_t *o = ring->items[i];
        if (!o || keyring_is_expired(o, now_ns))
            continue;
        if (o->type == TASK_KEY_OBJECT_KEY) {
            if (strcmp(o->key_type, type) == 0 &&
                strcmp(o->description, desc) == 0)
                return o;
            continue;
        }
        if (!direct_only && depth > 0) {
            task_key_object_t *nested =
                keyring_find_locked(o, type, desc, 0, depth - 1, now_ns);
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

    task_key_object_t *existing =
        keyring_find_locked(ring, type, desc, 1, 0, now);
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
     * reference on success (matching task_key_object_get_locked in the
     * na-kernel reference).  The serial-map membership does not hold a
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
        session ? keyring_find_locked(session, type, desc, 0,
                                      KEYRING_SEARCH_DEPTH, now)
                : NULL;
    if (!key) {
        task_key_object_t *user =
            keyring_resolve_locked(t, KEY_SPEC_USER_KEYRING, 0);
        if (user)
            key = keyring_find_locked(user, type, desc, 0, 1, now);
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
        keyring_unlink_from_serial_map_locked(old);
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
    o->expiry_ns = secs == 0 ? 0 : keyring_now_ns() + secs * 1000000000ULL;
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
        ring ? keyring_find_locked(ring, type, desc, 0,
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

void keyring_inherit(struct task_t *child, struct task_t *parent)
{
    if (!child || !parent || !parent->session_keyring)
        return;
    unsigned long flags = spin_lock_irqsave(&g_keyring_lock);
    task_key_object_t *ring = (task_key_object_t *)parent->session_keyring;
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

#include "ipc/keyring.h"

#include "core/lock.h"
#include "core/timekeeping.h"
#include "mm/slab.h"
#include "proc/proc.h"

/*
 * Narrow mechanical boundary for the Rust-owned keyring implementation.
 * This file intentionally owns no key/keyring state or policy.  The C
 * keyring.c remains the selectable reference implementation; a Rust-selected
 * profile links this bridge instead and gets the same public keyring_* ABI.
 */

static spinlock_t g_keyring_rust_lock = SPINLOCK_INIT;

extern key_serial_t a20_rust_keyring_add_key(const char *, const char *,
                                              const void *, size_t,
                                              key_serial_t);
extern key_serial_t a20_rust_keyring_request_key(const char *, const char *,
                                                 key_serial_t);
extern key_serial_t a20_rust_keyring_get_keyring_id(key_serial_t, int);
extern key_serial_t a20_rust_keyring_join_session(const char *);
extern int a20_rust_keyring_chown(key_serial_t, int, int);
extern int a20_rust_keyring_setperm(key_serial_t, uint32_t);
extern int a20_rust_keyring_set_timeout(key_serial_t, unsigned long);
extern int a20_rust_keyring_link(key_serial_t, key_serial_t);
extern int a20_rust_keyring_unlink(key_serial_t, key_serial_t);
extern int a20_rust_keyring_clear(key_serial_t);
extern int a20_rust_keyring_invalidate(key_serial_t);
extern key_serial_t a20_rust_keyring_search(key_serial_t, const char *,
                                            const char *);
extern long a20_rust_keyring_describe(key_serial_t, char *, size_t);
extern long a20_rust_keyring_read(key_serial_t, void *, size_t);
extern void a20_rust_keyring_inherit(struct task_t *, struct task_t *);
extern void a20_rust_keyring_release_task(struct task_t *);

void *a20_keyring_current_task(void)
{
    return proc_current();
}

/* Snapshot the add-key credentials before Rust allocates the object/payload.
 * A missing task or invalid output pointer performs no partial write. */
void *a20_keyring_current_cred_snapshot(int64_t *euid_out, int64_t *egid_out)
{
    if (!euid_out || !egid_out)
        return NULL;
    task_t *t = proc_current();
    if (!t)
        return NULL;
    *euid_out = t->cred.euid;
    *egid_out = t->cred.egid;
    return t;
}

int64_t a20_keyring_task_uid(void *opaque)
{
    task_t *t = opaque;
    return t ? t->cred.uid : -1;
}

int64_t a20_keyring_task_euid(void *opaque)
{
    task_t *t = opaque;
    return t ? t->cred.euid : -1;
}

int64_t a20_keyring_task_gid(void *opaque)
{
    task_t *t = opaque;
    return t ? t->cred.gid : -1;
}

int64_t a20_keyring_task_egid(void *opaque)
{
    task_t *t = opaque;
    return t ? t->cred.egid : -1;
}

int a20_keyring_task_has_cap(void *opaque, int cap)
{
    task_t *t = opaque;
    return t && proc_has_cap(t, cap);
}

void *a20_keyring_session_get(void *opaque)
{
    task_t *t = opaque;
    return t ? t->session_keyring : NULL;
}

void a20_keyring_session_set(void *opaque, void *ring)
{
    task_t *t = opaque;
    if (t)
        t->session_keyring = ring;
}

uint64_t a20_keyring_lock(void)
{
    return spin_lock_irqsave(&g_keyring_rust_lock);
}

/* Combine two tiny Rust FFI crossings on syscall paths. A missing current
 * task returns without acquiring the lock; successful calls transfer the
 * irq-save flags to Rust's RAII LockGuard for every early-return path. */
void *a20_keyring_lock_current(uint64_t *flags_out)
{
    if (!flags_out)
        return NULL;
    task_t *t = proc_current();
    if (!t)
        return NULL;
    *flags_out = spin_lock_irqsave(&g_keyring_rust_lock);
    return t;
}

void a20_keyring_unlock(uint64_t flags)
{
    spin_unlock_irqrestore(&g_keyring_rust_lock, flags);
}

uint64_t a20_keyring_now_ns(void)
{
    uint64_t ts[2];
    timekeeping_get_monotonic(ts);
    return ts[0] * 1000000000ULL + ts[1];
}

void *a20_keyring_alloc(size_t size)
{
    return kmalloc(size);
}

void a20_keyring_free(void *ptr)
{
    kfree(ptr);
}

key_serial_t keyring_add_key(const char *type, const char *desc,
                             const void *payload, size_t plen,
                             key_serial_t ringid)
{
    return a20_rust_keyring_add_key(type, desc, payload, plen, ringid);
}

key_serial_t keyring_request_key(const char *type, const char *desc,
                                 key_serial_t dest_ringid)
{
    return a20_rust_keyring_request_key(type, desc, dest_ringid);
}

key_serial_t keyring_get_keyring_id(key_serial_t id, int create)
{
    return a20_rust_keyring_get_keyring_id(id, create);
}

key_serial_t keyring_join_session(const char *name)
{
    return a20_rust_keyring_join_session(name);
}

int keyring_chown(key_serial_t id, int uid, int gid)
{
    return a20_rust_keyring_chown(id, uid, gid);
}

int keyring_setperm(key_serial_t id, uint32_t perm)
{
    return a20_rust_keyring_setperm(id, perm);
}

int keyring_set_timeout(key_serial_t id, unsigned long secs)
{
    return a20_rust_keyring_set_timeout(id, secs);
}

int keyring_link(key_serial_t id, key_serial_t ringid)
{
    return a20_rust_keyring_link(id, ringid);
}

int keyring_unlink(key_serial_t id, key_serial_t ringid)
{
    return a20_rust_keyring_unlink(id, ringid);
}

int keyring_clear(key_serial_t id)
{
    return a20_rust_keyring_clear(id);
}

int keyring_invalidate(key_serial_t id)
{
    return a20_rust_keyring_invalidate(id);
}

key_serial_t keyring_search(key_serial_t ringid, const char *type,
                            const char *desc)
{
    return a20_rust_keyring_search(ringid, type, desc);
}

long keyring_describe(key_serial_t id, char *buf, size_t bufsz)
{
    return a20_rust_keyring_describe(id, buf, bufsz);
}

long keyring_read(key_serial_t id, void *buf, size_t bufsz)
{
    return a20_rust_keyring_read(id, buf, bufsz);
}

void keyring_inherit(struct task_t *child, struct task_t *parent)
{
    a20_rust_keyring_inherit(child, parent);
}

void keyring_release_task(struct task_t *t)
{
    a20_rust_keyring_release_task(t);
}

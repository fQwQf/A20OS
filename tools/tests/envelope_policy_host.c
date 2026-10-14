#include <stdint.h>
#include <stdio.h>

#define EPERM 1
#define EACCES 13
#define EINVAL 22
#define EMFILE 24
#define READ_RIGHT 1ull
#define WRITE_RIGHT 2ull
#define DENY_NONE 0
#define DENY_RIGHTS 1
#define DENY_OPS 2
#define DENY_DATA 3

int a20_envelope_policy_expire(int *, uint64_t, uint64_t);
int a20_envelope_policy_charge_acquire(uint64_t *);
int a20_envelope_policy_charge_propagation(uint32_t, uint32_t, uint64_t *);
int a20_envelope_policy_charge_propagation_at(int *, uint64_t, uint64_t,
                                               uint32_t, uint32_t, uint64_t *);
int a20_envelope_policy_charge_use(uint64_t *, uint64_t *, uint64_t, int, int,
                                   uint64_t);

typedef struct {
    uint64_t vfile_identity;
    uint8_t obj_type;
    uint8_t reserved[7];
    uint64_t rights;
} env_shadow_t;
typedef struct {
    void *ptr;
    void *owner_task;
    uint32_t generation;
    int32_t owner_pid;
    uint8_t owner_live;
    uint8_t retired;
    uint16_t reserved;
} env_registry_slot_t;
_Static_assert(sizeof(env_shadow_t) == 24, "host shadow ABI");
_Static_assert(__builtin_offsetof(env_shadow_t, rights) == 16,
               "host shadow rights offset");
_Static_assert(sizeof(env_registry_slot_t) ==
                   (((2 * sizeof(void *) + 12 + __alignof__(void *) - 1) /
                     __alignof__(void *)) * __alignof__(void *)),
               "host registry slot size");
_Static_assert(__builtin_offsetof(env_registry_slot_t, generation) ==
                   2 * sizeof(void *), "host registry generation offset");
_Static_assert(__builtin_offsetof(env_registry_slot_t, owner_live) ==
                   2 * sizeof(void *) + 8, "host registry owner offset");
int a20_envelope_policy_shadow_init(env_shadow_t *, uint32_t);
int a20_envelope_policy_shadow_upsert(env_shadow_t *, uint32_t, uint64_t,
                                      uint8_t, uint64_t);
int a20_envelope_policy_acquire_shadow(uint64_t *, env_shadow_t *, uint32_t,
                                       uint64_t, uint8_t, uint64_t);
int a20_envelope_policy_shadow_lookup(const env_shadow_t *, uint32_t, uint64_t,
                                      uint64_t *, int32_t *);
int a20_envelope_policy_shadow_count(const env_shadow_t *, uint32_t);
int a20_envelope_policy_shadow_clear(env_shadow_t *, uint32_t);
int a20_envelope_policy_shadow_remove(env_shadow_t *, uint32_t, uint64_t);
int64_t a20_envelope_registry_insert(env_registry_slot_t *, uint32_t, void *, void *, int32_t);
void *a20_envelope_registry_lookup(env_registry_slot_t *, uint32_t, int64_t, int);
void *a20_envelope_registry_close_owner(env_registry_slot_t *, uint32_t, void *);
int a20_envelope_registry_remove(env_registry_slot_t *, uint32_t, void *);

static int legacy_shadow_upsert(env_shadow_t *entries, unsigned capacity,
                                uint64_t identity, uint8_t type,
                                uint64_t rights)
{
    int slot = -1;
    for (unsigned i = 0; i < capacity; ++i) {
        if (entries[i].vfile_identity == identity) return (int)i;
        if (slot < 0 && entries[i].vfile_identity == 0) slot = (int)i;
    }
    if (slot < 0) return -EMFILE;
    entries[slot] = (env_shadow_t){ .vfile_identity = identity,
                                    .obj_type = type, .rights = rights };
    return slot;
}

static int legacy_acquire_shadow(uint64_t *ops, env_shadow_t *entries,
                                 unsigned capacity, uint64_t identity,
                                 uint8_t type, uint64_t rights)
{
    if (*ops == 0) return -EACCES;
    --*ops;
    int slot = legacy_shadow_upsert(entries, capacity, identity, type, rights);
    return slot < 0 ? slot : 0;
}

static void legacy_shadow_init(env_shadow_t *entries, unsigned capacity)
{
    for (unsigned i = 0; i < capacity; ++i)
        entries[i] = (env_shadow_t){ .vfile_identity = 0 };
}

static int legacy_shadow_lookup(const env_shadow_t *entries, unsigned capacity,
                                uint64_t identity, uint64_t *rights,
                                int32_t *tracked)
{
    *rights = 0;
    *tracked = 0;
    for (unsigned i = 0; i < capacity; ++i) {
        if (entries[i].vfile_identity == identity) {
            *rights = entries[i].rights;
            *tracked = 1;
            break;
        }
    }
    return 0;
}

static int legacy_expire(int *expired, uint64_t deadline, uint64_t now)
{
    if (!*expired && deadline != 0 && now >= deadline)
        *expired = 1;
    return *expired ? -EACCES : 0;
}

static int legacy_propagation(uint32_t mask, uint32_t kind, uint64_t *ops)
{
    if (!(mask & (1u << kind))) return -EPERM;
    if (*ops == 0) return -EACCES;
    --*ops;
    return 0;
}

static int legacy_locked_send(int *expired, uint64_t deadline, uint64_t now,
                              uint32_t mask, uint32_t kind, uint64_t *ops)
{
    int rc = legacy_expire(expired, deadline, now);
    if (rc) return rc;
    return legacy_propagation(mask, kind, ops);
}

static int legacy_use(uint64_t *ops, uint64_t *data, uint64_t rights,
                      int tracked, int want_write, uint64_t bytes)
{
    if (tracked && want_write >= 0) {
        uint64_t need = want_write ? WRITE_RIGHT : READ_RIGHT;
        if (!(rights & need)) return -EACCES;
    }
    if (*ops == 0) return -EACCES;
    --*ops;
    if (bytes) {
        if (*data < bytes) return -EACCES;
        *data -= bytes;
    }
    return 0;
}

static int rust_use_errno(int result)
{
    return result == DENY_NONE ? 0 : result == -EINVAL ? -EINVAL : -EACCES;
}

int main(void)
{
    {
        enum { CAP = 1024 };
        env_registry_slot_t slots[CAP] = {0};
        void *first = (void *)(uintptr_t)0x1000;
        void *second = (void *)(uintptr_t)0x2000;
        void *owner_a = (void *)(uintptr_t)0x4100;
        void *owner_b = (void *)(uintptr_t)0x4200;
        int64_t old_id = a20_envelope_registry_insert(slots, CAP, first, owner_a, 41);
        if (old_id != 0 || a20_envelope_registry_lookup(slots, CAP, old_id, 1) != first)
            return 14;
        if (a20_envelope_registry_close_owner(slots, CAP, owner_b) != NULL ||
            a20_envelope_registry_lookup(slots, CAP, old_id, 1) != first)
            return 15;
        if (a20_envelope_registry_close_owner(slots, CAP, owner_a) != first ||
            a20_envelope_registry_lookup(slots, CAP, old_id, 1) != NULL)
            return 16;
        if (a20_envelope_registry_remove(slots, CAP, first) != 0)
            return 17;
        int64_t new_id = a20_envelope_registry_insert(slots, CAP, second, owner_b, 41);
        if (new_id != 1024 || a20_envelope_registry_lookup(slots, CAP, old_id, 0) != NULL ||
            a20_envelope_registry_lookup(slots, CAP, new_id, 1) != second)
            return 18;
    }
    {
        enum { CAP = 3 };
        env_shadow_t c[CAP], r[CAP];
        uint64_t c_ops = 0, r_ops = 0;
        legacy_shadow_init(c, CAP);
        if (a20_envelope_policy_shadow_init(r, CAP) != 0) return 7;
        const uint64_t ids[] = {0x1001, 0x2002, 0x3003, 0x2002,
                                0x1001, 0x4004};
        const uint8_t types[] = {1, 3, 7, 5, 8, 2};
        const uint64_t rights[] = {1, 3, 5, 7, 9, 11};
        for (unsigned i = 0; i < sizeof(ids) / sizeof(ids[0]); ++i) {
            c_ops = r_ops = (i == 0 ? 0 : 4);
            int cr = legacy_acquire_shadow(&c_ops, c, CAP, ids[i],
                                           types[i], rights[i]);
            int rr = a20_envelope_policy_acquire_shadow(
                &r_ops, r, CAP, ids[i], types[i], rights[i]);
            if (cr != rr || c_ops != r_ops) return 8;
            for (unsigned j = 0; j < CAP; ++j) {
                if (c[j].vfile_identity != r[j].vfile_identity || c[j].obj_type != r[j].obj_type ||
                    c[j].rights != r[j].rights) return 9;
            }
        }
        for (uint64_t identity = 0x1000; identity < 0x5000; identity += 0x100) {
            uint64_t crights = 0, rrights = 0;
            int32_t ctracked = 0, rtracked = 0;
            if (legacy_shadow_lookup(c, CAP, identity, &crights, &ctracked) !=
                    a20_envelope_policy_shadow_lookup(r, CAP, identity, &rrights,
                                                      &rtracked) ||
                crights != rrights || ctracked != rtracked) return 10;
        }
        int expected_count = 0;
        for (unsigned i = 0; i < CAP; ++i) expected_count += c[i].vfile_identity != 0;
        if (a20_envelope_policy_shadow_count(r, CAP) != expected_count) return 11;
        legacy_shadow_init(c, CAP);
        if (a20_envelope_policy_shadow_clear(r, CAP) != 0) return 12;
        for (unsigned i = 0; i < CAP; ++i)
            if (c[i].vfile_identity != r[i].vfile_identity ||
                r[i].vfile_identity != 0 || r[i].rights != 0)
                return 13;
    }
    {
        env_shadow_t legacy[2], rust[2];
        legacy_shadow_init(legacy, 2);
        if (a20_envelope_policy_shadow_init(rust, 2) != 0) return 20;
        if (legacy_shadow_upsert(legacy, 2, 0x11, 4, 1) != 0 ||
            legacy_shadow_upsert(legacy, 2, 0x22, 4, 2) != 1 ||
            a20_envelope_policy_shadow_upsert(rust, 2, 0x11, 4, 1) != 0 ||
            a20_envelope_policy_shadow_upsert(rust, 2, 0x22, 4, 2) != 1)
            return 21;
        if (legacy_shadow_upsert(legacy, 2, 0x11, 4, 3) != 0 ||
            legacy_shadow_upsert(legacy, 2, 0x11, 4, 0) != 0 ||
            a20_envelope_policy_shadow_upsert(rust, 2, 0x11, 4, 3) != 0 ||
            a20_envelope_policy_shadow_upsert(rust, 2, 0x11, 4, 0) != 0)
            return 25;
        uint64_t legacy_rights = 0, rust_rights = 0;
        int32_t legacy_tracked = 0, rust_tracked = 0;
        uint64_t legacy_ops = 1, rust_ops = 1;
        uint64_t legacy_data = 8, rust_data = 8;
        if (legacy_shadow_lookup(legacy, 2, 0x11, &legacy_rights, &legacy_tracked) != 0 ||
            a20_envelope_policy_shadow_lookup(rust, 2, 0x11, &rust_rights, &rust_tracked) != 0 ||
            legacy_rights != 1 || rust_rights != legacy_rights ||
            legacy_tracked != 1 || rust_tracked != legacy_tracked)
            return 26;
        int legacy_write = legacy_use(&legacy_ops, &legacy_data, legacy_rights, legacy_tracked, 1, 0);
        int rust_write = rust_use_errno(a20_envelope_policy_charge_use(
            &rust_ops, &rust_data, rust_rights, rust_tracked, 1, 0));
        if (legacy_write != -EACCES || legacy_write != rust_write ||
            legacy_ops != 1 || rust_ops != 1)
            return 27;
        uint64_t rights = 0;
        int32_t tracked = 0;
        if (a20_envelope_policy_shadow_lookup(rust, 2, 0x33, &rights, &tracked) != 0 ||
            rights != 0 || tracked != 0)
            return 22;
        /* A shared envelope may see the same vfile through several tasks.
         * Reacquiring it with broader or narrower requested rights must not
         * mutate the first grant observed by existing aliases. */
        if (a20_envelope_policy_shadow_upsert(rust, 2, 0x11, 4, 3) != 0 ||
            a20_envelope_policy_shadow_upsert(rust, 2, 0x11, 4, 0) != 0 ||
            a20_envelope_policy_shadow_lookup(rust, 2, 0x11, &rights, &tracked) != 0 ||
            rights != 1 || tracked != 1)
            return 24;
        if (a20_envelope_policy_shadow_remove(rust, 2, 0x11) != 1 ||
            a20_envelope_policy_shadow_count(rust, 2) != 1 ||
            a20_envelope_policy_shadow_upsert(rust, 2, 0x33, 4, 4) != 0 ||
            a20_envelope_policy_shadow_remove(rust, 2, 0x22) != 1 ||
            a20_envelope_policy_shadow_count(rust, 2) != 1)
            return 23;
    }
    const uint32_t masks[] = {0, 1, 1u << 3, 0xffffffffu};
    for (int was_expired = 0; was_expired <= 1; ++was_expired) {
        for (uint64_t deadline = 0; deadline <= 3; ++deadline) {
            for (uint64_t now = 0; now <= 4; ++now) {
                int c = was_expired, r = was_expired;
                int cr = legacy_expire(&c, deadline, now);
                int rr = a20_envelope_policy_expire(&r, deadline, now) ? -EACCES : 0;
                if (cr != rr || c != r) return 1;
            }
        }
    }
    /* Exercise the exact Rust transition called from the production
     * fdtable-locked SCM callback: preflight succeeds, then the deadline
     * passes before the callback. Expiry must deny before op debit. */
    {
        const uint32_t send_masks[] = {0, 1u << 3};
        for (unsigned m = 0; m < 2; ++m) {
            for (uint64_t initial_ops = 0; initial_ops <= 2; ++initial_ops) {
                int c_expired = 0, r_expired = 0;
                uint64_t c_ops = initial_ops, r_ops = initial_ops;
                if (legacy_expire(&c_expired, 9, 8) != 0 ||
                    a20_envelope_policy_expire(&r_expired, 9, 8) != 0)
                    return 4;
                int c_rc = legacy_locked_send(&c_expired, 9, 9,
                                               send_masks[m], 3, &c_ops);
                int r_rc = a20_envelope_policy_charge_propagation_at(
                    &r_expired, 9, 9, send_masks[m], 3, &r_ops);
                if (c_rc != r_rc || c_expired != 1 ||
                    c_expired != r_expired || c_ops != initial_ops ||
                    c_ops != r_ops) {
                    fprintf(stderr, "locked send mismatch mask=%u ops=%llu rc=%d/%d expired=%d/%d remaining=%llu/%llu\n",
                            m, (unsigned long long)initial_ops, c_rc, r_rc,
                            c_expired, r_expired,
                            (unsigned long long)c_ops,
                            (unsigned long long)r_ops);
                    return 5;
                }
                c_expired = 0; r_expired = 0;
                c_ops = initial_ops; r_ops = initial_ops;
                c_rc = legacy_locked_send(&c_expired, 9, 8,
                                          send_masks[m], 3, &c_ops);
                r_rc = a20_envelope_policy_charge_propagation_at(
                    &r_expired, 9, 8, send_masks[m], 3, &r_ops);
                if (c_rc != r_rc || c_expired != r_expired ||
                    c_ops != r_ops)
                    return 6;
            }
        }
    }
    for (unsigned m = 0; m < sizeof(masks) / sizeof(masks[0]); ++m) {
        for (uint32_t kind = 0; kind < 32; ++kind) {
            for (uint64_t initial_ops = 0; initial_ops <= 2; ++initial_ops) {
                uint64_t c = initial_ops, r = initial_ops;
                int cr = legacy_propagation(masks[m], kind, &c);
                int rr = a20_envelope_policy_charge_propagation(masks[m], kind, &r);
                if (cr != rr || c != r) return 2;
            }
        }
    }
    const uint64_t rights_values[] = {0, READ_RIGHT, WRITE_RIGHT,
                                      READ_RIGHT | WRITE_RIGHT};
    const uint64_t byte_values[] = {0, 1, 3, 4};
    for (int tracked = 0; tracked <= 1; ++tracked) {
        for (int direction = -1; direction <= 1; ++direction) {
            for (unsigned ri = 0; ri < 4; ++ri) {
                for (uint64_t initial_ops = 0; initial_ops <= 2; ++initial_ops) {
                    for (uint64_t initial_data = 0; initial_data <= 4; ++initial_data) {
                        for (unsigned bi = 0; bi < 4; ++bi) {
                            uint64_t co = initial_ops, ro = initial_ops;
                            uint64_t cd = initial_data, rd = initial_data;
                            int cr = legacy_use(&co, &cd, rights_values[ri], tracked,
                                                direction, byte_values[bi]);
                            int decision = a20_envelope_policy_charge_use(
                                &ro, &rd, rights_values[ri], tracked, direction,
                                byte_values[bi]);
                            int rr = rust_use_errno(decision);
                            if (cr != rr || co != ro || cd != rd) return 3;
                        }
                    }
                }
            }
        }
    }
    puts("ENVELOPE_POLICY: PASS production shadow Rust/C differential; registry owner-close/reuse/stale-id; locked-send expiry-no-debit; propagation/quota/rights/data");
    return 0;
}

#ifndef _KERNEL_IPC_ENVELOPE_POLICY_H
#define _KERNEL_IPC_ENVELOPE_POLICY_H

#include <stdint.h>

#define A20_ENV_POLICY_DENY_NONE   0
#define A20_ENV_POLICY_DENY_RIGHTS 1
#define A20_ENV_POLICY_DENY_OPS    2
#define A20_ENV_POLICY_DENY_DATA   3

/* Shadow records remain embedded in the C-owned envelope allocation. Rust
 * owns their bounded-table transitions; the caller holds e->lock throughout.
 * The key is the unique lifetime identity of the vfile, not a reusable local
 * fd. Zero is reserved for an unused slot. */
typedef struct a20_env_shadow_state {
    uint64_t vfile_identity;  /* zero is empty; stable through dup/fork aliases */
    uint8_t obj_type;
    uint8_t reserved[7];
    uint64_t rights;
} a20_env_shadow_state_t;

#define A20_ENV_REGISTRY_SLOTS 1024u
typedef struct a20_env_registry_slot {
    void *ptr;
    void *owner_task;
    uint32_t generation;
    int32_t owner_pid;
    uint8_t owner_live;
    uint8_t retired;
    uint16_t reserved;
} a20_env_registry_slot_t;

_Static_assert(sizeof(a20_env_shadow_state_t) == 24,
               "Rust envelope shadow ABI");
_Static_assert(__builtin_offsetof(a20_env_shadow_state_t, rights) == 16,
               "Rust envelope shadow rights offset");
_Static_assert(sizeof(a20_env_registry_slot_t) ==
                   (((2 * sizeof(void *) + 12 + __alignof__(void *) - 1) /
                     __alignof__(void *)) * __alignof__(void *)),
               "Rust envelope registry slot size");
_Static_assert(__builtin_offsetof(a20_env_registry_slot_t, generation) ==
                   2 * sizeof(void *), "Rust envelope registry generation offset");
_Static_assert(__builtin_offsetof(a20_env_registry_slot_t, owner_live) ==
                   2 * sizeof(void *) + 8, "Rust envelope registry owner offset");

/* FFI contract: every pointer argument must refer to an initialized, naturally
 * aligned object of the declared type and remain valid for the whole call.
 * The two mutable counters passed to charge_use must be distinct,
 * nonoverlapping objects. Callers serialize all mutable envelope state with
 * e->lock; these helpers do not acquire locks or perform side effects. Rights
 * use the A20_RIGHT_READ/A20_RIGHT_WRITE bit positions from ipc/ipc.h. */
int a20_envelope_policy_expire(int *expired, uint64_t expire_tick,
                               uint64_t now);
int a20_envelope_policy_charge_acquire(uint64_t *remaining_ops);
int a20_envelope_policy_charge_propagation(uint32_t propagation_types,
                                           uint32_t kind,
                                           uint64_t *remaining_ops);
int a20_envelope_policy_charge_propagation_at(int *expired,
                                              uint64_t expire_tick,
                                              uint64_t now,
                                              uint32_t propagation_types,
                                              uint32_t kind,
                                              uint64_t *remaining_ops);
int a20_envelope_policy_charge_use(uint64_t *remaining_ops,
                                   uint64_t *remaining_data,
                                   uint64_t shadow_rights, int tracked,
                                   int want_write, uint64_t bytes);
int a20_envelope_policy_shadow_init(a20_env_shadow_state_t *entries,
                                    uint32_t capacity);
int a20_envelope_policy_shadow_upsert(a20_env_shadow_state_t *entries,
                                      uint32_t capacity, uint64_t identity,
                                      uint8_t obj_type, uint64_t rights);
/* Preserves the legacy order: op is charged before a full-table failure. */
int a20_envelope_policy_acquire_shadow(uint64_t *remaining_ops,
                                       a20_env_shadow_state_t *entries,
                                       uint32_t capacity, uint64_t identity,
                                       uint8_t obj_type, uint64_t rights);
int a20_envelope_policy_shadow_lookup(const a20_env_shadow_state_t *entries,
                                      uint32_t capacity, uint64_t identity,
                                      uint64_t *rights, int32_t *tracked);
int a20_envelope_policy_shadow_count(const a20_env_shadow_state_t *entries,
                                     uint32_t capacity);
int a20_envelope_policy_shadow_clear(a20_env_shadow_state_t *entries,
                                     uint32_t capacity);
int a20_envelope_policy_shadow_remove(a20_env_shadow_state_t *entries,
                                      uint32_t capacity, uint64_t identity);

/* These bounded-registry transitions are called only while the C registry
 * spinlock is held. The registry is a weak index: owner/task refs keep the
 * pointed-to object alive. IDs encode generation above the 10-bit slot. */
int64_t a20_envelope_registry_insert(a20_env_registry_slot_t *slots,
                                     uint32_t capacity, void *ptr,
                                     void *owner_task, int32_t owner_pid);
void *a20_envelope_registry_lookup(a20_env_registry_slot_t *slots,
                                   uint32_t capacity, int64_t id,
                                   int require_owner);
void *a20_envelope_registry_at(a20_env_registry_slot_t *slots,
                              uint32_t capacity, uint32_t index);
void *a20_envelope_registry_close_owner(a20_env_registry_slot_t *slots,
                                        uint32_t capacity, void *owner_task);
int a20_envelope_registry_remove(a20_env_registry_slot_t *slots,
                                 uint32_t capacity, void *ptr);

#endif

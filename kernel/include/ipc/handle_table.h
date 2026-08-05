/*
 * A20OS core — handle table (capability store).
 *
 * Ownership principle: the handle table is an internal capability
 * mechanism; the Native ABI syscall layer wraps it.  Internal code must
 * include this header, never anything under abi/.
 */
#ifndef _IPC_HANDLE_TABLE_H
#define _IPC_HANDLE_TABLE_H

#include "core/types.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "ipc/ipc.h"

/* Temporal sweeper cadence (docs/native-abi/03-handle.md §2.6.4). */
#define A20_SWEEP_INTERVAL_TICKS  (TICKS_PER_SEC / 10 > 0 ? TICKS_PER_SEC / 10 : 1)

/* Handle-table sizing (per-task quota, M2). */
#define A20_HT_INITIAL_CAP    256
#define A20_HT_MAX_CAP        65536
#define A20_HT_DEFAULT_QUOTA  4096
#define A20_HT_GROWTH_FACTOR  2

/* Temporal capability flags. */
#define A20_TEMPORAL_EXPIRY_ABSOLUTE  (1u << 0)
#define A20_TEMPORAL_OP_COUNT         (1u << 1)
#define A20_TEMPORAL_AUTO_CLOSE       (1u << 2)

/* Handle states (docs/native-abi/03-handle.md §3.1). */
typedef enum a20_handle_state {
    A20_HS_FREE      = 0,
    A20_HS_ACTIVE    = 1,
    A20_HS_EXPIRED   = 2,
    A20_HS_CLOSING   = 3,
} a20_handle_state_t;

typedef struct a20_handle_entry {
    void           *object;
    uint16_t        type;          /* a20_object_type_t */
    uint16_t        _pad;
    a20_rights_t    rights;
    uint64_t        expiry_tick;
    uint32_t        remaining_ops;
    uint32_t        temporal_flags;
    uint8_t         security_label; /* L=0, M=1, H=2 (Bell-LaPadula) */
    uint8_t         state;          /* a20_handle_state_t */
    uint8_t         _pad2[6];
} a20_handle_entry_t;

struct a20_ht_internal;

/*
 * Native handle lifetime invariants:
 * - ACTIVE entries are usable only when the type, rights, temporal limits,
 *   and security label admit the requested operation.
 * - CLOSING and EXPIRED entries reject new lookups; FREE entries own no object
 *   reference.
 * - install/dup/transfer may preserve or reduce rights and temporal limits,
 *   but must never make a capability more permissive.
 * - NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX: static gates require evidence
 *   for rights downgrade, temporal rights, security labels, close/dup/transfer,
 *   and partial delivery before this model can be marked complete.
 * - The table lock protects entry allocation, state transitions, operation
 *   count consumption, the free bitmap, and count/free_hint.
 */

/* ---- Handle table lifecycle ---- */

struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);
struct a20_ht_internal *a20_ht_get_ref(struct a20_ht_internal *ht);
void a20_ht_put_ref(struct a20_ht_internal *ht);

/* ---- Handle operations ---- */

int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
                                     uint16_t type, a20_rights_t rights,
                                     uint64_t expiry_tick,
                                     uint32_t remaining_ops,
                                     uint32_t temporal_flags,
                                     uint8_t security_label);
int64_t a20_handle_install_at_temporal(struct a20_ht_internal *ht,
                                       a20_handle_t slot, void *object,
                                       uint16_t type, a20_rights_t rights,
                                       uint64_t expiry_tick,
                                       uint32_t remaining_ops,
                                       uint32_t temporal_flags,
                                       uint8_t security_label);
int64_t a20_handle_lookup_internal(struct a20_ht_internal *ht, a20_handle_t h,
                                     uint16_t expected_type,
                                     a20_rights_t required_rights,
                                     a20_handle_entry_t *out);
int64_t a20_handle_lookup_ref_internal(struct a20_ht_internal *ht,
                                        a20_handle_t h,
                                        uint16_t expected_type,
                                        a20_rights_t required_rights,
                                        a20_handle_entry_t *out);
int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h);
int64_t a20_handle_reserve_many(struct a20_ht_internal *ht,
                                a20_handle_t *handles, uint32_t count);
void a20_handle_abort_reserved(struct a20_ht_internal *ht,
                               a20_handle_t *handles, uint32_t count);
int64_t a20_handle_commit_reserved_temporal(struct a20_ht_internal *ht,
                                            a20_handle_t h, void *object,
                                            uint16_t type, a20_rights_t rights,
                                            uint64_t expiry_tick,
                                            uint32_t remaining_ops,
                                            uint32_t temporal_flags,
                                            uint8_t security_label);
int64_t a20_handle_set_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                uint64_t expiry_tick, uint32_t remaining_ops,
                                uint32_t temporal_flags);
int64_t a20_handle_get_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                uint64_t *expiry_tick, uint32_t *remaining_ops,
                                uint32_t *temporal_flags);
int64_t a20_handle_set_label(struct a20_ht_internal *ht, a20_handle_t h,
                             uint8_t label);

/* ---- Per-task accessors ---- */

struct a20_ht_internal *task_get_a20_ht(task_t *t);
struct a20_ht_internal *task_get_a20_ht_ref(task_t *t);
uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);

/* Checkpoint-based signal simulation (no async signals on the native ABI). */
void     a20_ht_sig_pend(struct a20_ht_internal *ht, int sig);
uint64_t a20_ht_sig_take(struct a20_ht_internal *ht);
uint64_t a20_ht_sig_blocked(struct a20_ht_internal *ht);
uint64_t a20_ht_sig_set_blocked(struct a20_ht_internal *ht, uint64_t mask);

/* Temporal sweeper. */
void a20_temporal_sweep(struct a20_ht_internal *ht);
void a20_temporal_sweep_all(void);

#endif /* _IPC_HANDLE_TABLE_H */

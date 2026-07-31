#ifndef _ABI_NATIVE_HANDLE_TABLE_INTERNAL_H
#define _ABI_NATIVE_HANDLE_TABLE_INTERNAL_H

#include "core/types.h"
#include "core/timer.h"
#include "proc/proc.h"
#include "abi/native/types.h"
#include "abi/native/rights.h"

/* Temporal sweeper cadence (docs/native-abi/03-handle.md §2.6.4).  The
 * sweep is deadline-driven: set_temporal notes the next interesting tick
 * and each sweep re-arms the following one via sched_note_timer_deadline,
 * so no per-task alarm is ever involved. */
#define A20_SWEEP_INTERVAL_TICKS  (TICKS_PER_SEC / 10 > 0 ? TICKS_PER_SEC / 10 : 1)

struct a20_ht_internal;

/*
 * Native handle lifetime invariants:
 * - A handle is a process-local index into a20_ht_internal. The entry owns a
 *   typed object pointer plus the rights, temporal limits, security label, and
 *   state that gate access to that object.
 * - ACTIVE entries are usable only when type matches, required rights are a
 *   subset of effective rights, temporal constraints have not expired, and the
 *   Bell-LaPadula label check for the requested access succeeds.
 * - CLOSING entries reject new lookups while close/transfer cleanup runs.
 *   EXPIRED entries reject access until swept or explicitly removed. FREE
 *   entries have object == NULL and do not own object references.
 * - install/dup/transfer may only preserve or reduce rights and temporal limits;
 *   no operation may refresh an expiry, operation count, or security label into
 *   a more permissive state.
 * - NATIVE_HANDLE_CAPABILITY_CONSISTENCY_MATRIX: static gates require evidence
 *   for rights downgrade, temporal rights, security labels, close/dup/transfer,
 *   and partial delivery before this model can be marked complete.
 * - The handle table lock protects entry allocation, state transitions,
 *   remaining_ops consumption, free_bitmap, and count/free_hint. Object-specific
 *   lifetime rules are handled by the subsystem that owns the object pointer.
 */

struct a20_ht_internal *a20_ht_create(void);
void a20_ht_destroy(struct a20_ht_internal *ht);
struct a20_ht_internal *a20_ht_get_ref(struct a20_ht_internal *ht);
void a20_ht_put_ref(struct a20_ht_internal *ht);

int64_t a20_handle_install(struct a20_ht_internal *ht, void *object,
                           uint16_t type, a20_rights_t rights);
int64_t a20_handle_install_temporal(struct a20_ht_internal *ht, void *object,
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

int a20_object_is_vfile_backed(uint16_t type);
void a20_object_ref(void *object, uint16_t type);
void a20_object_release(void *object, uint16_t type);

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

struct a20_ht_internal *task_get_a20_ht(task_t *t);
struct a20_ht_internal *task_get_a20_ht_ref(task_t *t);
uint8_t a20_ht_get_label(struct a20_ht_internal *ht);
void a20_ht_set_label(struct a20_ht_internal *ht, uint8_t label);
int64_t a20_handle_set_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                uint64_t expiry_tick, uint32_t remaining_ops,
                                uint32_t temporal_flags);
int64_t a20_handle_get_temporal(struct a20_ht_internal *ht, a20_handle_t h,
                                uint64_t *expiry_tick, uint32_t *remaining_ops,
                                uint32_t *temporal_flags);
int64_t a20_handle_set_label(struct a20_ht_internal *ht, a20_handle_t h,
                             uint8_t label);
void a20_temporal_sweep(struct a20_ht_internal *ht);
void a20_temporal_sweep_all(void);

#endif

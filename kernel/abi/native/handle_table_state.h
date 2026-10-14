#ifndef A20_ABI_NATIVE_HANDLE_TABLE_STATE_H
#define A20_ABI_NATIVE_HANDLE_TABLE_STATE_H

#include <stddef.h>
#include <stdint.h>

typedef struct a20_rust_handle_table_state a20_rust_handle_table_state_t;
typedef void *(*a20_handle_state_alloc_fn)(size_t bytes);
typedef void (*a20_handle_state_free_fn)(void *ptr);

size_t a20_rust_handle_table_state_size(void);
size_t a20_rust_handle_table_state_align(void);

/*
 * Init/fini run with exclusive access. Init buffers are allocated by C, then
 * become owned by Rust state. Fini requires all slots already detached and
 * released; after success C frees the opaque state storage itself. Calling
 * state functions after fini is invalid; fini consumes the state exactly once.
 */
int32_t a20_rust_handle_table_state_init(
    a20_rust_handle_table_state_t *storage,
    uint8_t *entries, size_t entry_size, size_t entry_align, uint32_t capacity,
    uint64_t *bitmap, uint32_t bitmap_words,
    uint32_t max_capacity, uint32_t max_handles,
    a20_handle_state_alloc_fn alloc, a20_handle_state_free_fn free_fn);
int32_t a20_rust_handle_table_state_fini(a20_rust_handle_table_state_t *storage);

/* All mutating calls require the caller to hold the IRQ-saving table lock.
 * The allocator callbacks passed to init are called synchronously from growth
 * while that lock is held. They must be non-sleeping and non-reentrant, as the
 * existing C ht_grow() already requires. */
int32_t a20_rust_handle_table_reserve_many(
    a20_rust_handle_table_state_t *state, uint32_t count, uint32_t *out_slots);
int64_t a20_rust_handle_table_install(
    a20_rust_handle_table_state_t *state, void *object, uint16_t object_type,
    uint64_t rights, uint64_t expiry_tick, uint32_t remaining_ops,
    uint32_t temporal_flags, uint8_t security_label);
int64_t a20_rust_handle_table_install_at(
    a20_rust_handle_table_state_t *state, uint32_t slot, void *object,
    uint16_t object_type, uint64_t rights, uint64_t expiry_tick,
    uint32_t remaining_ops, uint32_t temporal_flags, uint8_t security_label);
int32_t a20_rust_handle_table_commit(
    a20_rust_handle_table_state_t *state, uint32_t slot, void *object,
    uint16_t object_type, uint64_t rights, uint64_t expiry_tick,
    uint32_t remaining_ops, uint32_t temporal_flags, uint8_t security_label);
uint32_t a20_rust_handle_table_abort_many(
    a20_rust_handle_table_state_t *state, const uint32_t *slots, uint32_t count);
int32_t a20_rust_handle_table_remove(
    a20_rust_handle_table_state_t *state, uint32_t slot,
    void **object, uint16_t *object_type);
int64_t a20_rust_handle_table_lookup(
    a20_rust_handle_table_state_t *state, uint32_t slot,
    uint16_t expected_type, uint64_t required_rights, uint64_t now, void *out);
int32_t a20_rust_handle_table_set_temporal(
    a20_rust_handle_table_state_t *state, uint32_t slot, uint64_t expiry_tick,
    uint32_t remaining_ops, uint32_t temporal_flags);
int32_t a20_rust_handle_table_get_temporal(
    a20_rust_handle_table_state_t *state, uint32_t slot, uint64_t *expiry_tick,
    uint32_t *remaining_ops, uint32_t *temporal_flags);
int32_t a20_rust_handle_table_set_label(
    a20_rust_handle_table_state_t *state, uint32_t slot, uint8_t label);
int32_t a20_rust_handle_table_expire(
    a20_rust_handle_table_state_t *state, uint32_t slot, uint64_t now,
    void **object, uint16_t *object_type);
int32_t a20_rust_handle_table_destroy_next(
    a20_rust_handle_table_state_t *state, uint32_t *cursor,
    void **object, uint16_t *object_type);
uint32_t a20_rust_handle_table_capacity(const a20_rust_handle_table_state_t *state);
uint32_t a20_rust_handle_table_used(const a20_rust_handle_table_state_t *state);
int32_t a20_rust_handle_table_set_quota(
    a20_rust_handle_table_state_t *state, uint32_t quota);

#endif

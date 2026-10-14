#include "kernel/abi/native/handle_table_state.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct test_handle_entry {
    void *object;
    uint16_t object_type;
    uint16_t pad;
    uint64_t rights;
    uint64_t expiry_tick;
    uint32_t remaining_ops;
    uint32_t temporal_flags;
    uint8_t security_label;
    uint8_t state;
    uint8_t pad2[6];
} test_handle_entry_t;

_Static_assert(sizeof(test_handle_entry_t) == 48, "host test Entry ABI mismatch");
_Static_assert(_Alignof(test_handle_entry_t) == 8, "host test Entry alignment mismatch");

static unsigned alloc_calls;
static unsigned successful_allocations;
static unsigned free_calls;
static unsigned live_allocations;
static unsigned fail_call;

static void *test_alloc(size_t bytes)
{
    alloc_calls++;
    if (fail_call != 0 && alloc_calls == fail_call)
        return NULL;
    void *ptr = malloc(bytes);
    if (ptr) {
        live_allocations++;
        successful_allocations++;
    }
    return ptr;
}

static void test_free(void *ptr)
{
    if (!ptr)
        return;
    assert(live_allocations != 0);
    live_allocations--;
    free_calls++;
    free(ptr);
}

int main(void)
{
    enum { INITIAL_CAPACITY = 2, MAX_CAPACITY = 8 };
    const uintptr_t object_a = 0x1000;
    const uintptr_t object_b = 0x2000;
    const uintptr_t object_c = 0x3000;
    void *detached = NULL;
    uint16_t detached_type = 0;
    const size_t entry_size = sizeof(test_handle_entry_t);
    const size_t state_size = a20_rust_handle_table_state_size();
    const size_t state_align = a20_rust_handle_table_state_align();
    assert(state_size != 0 && state_align >= sizeof(void *));

    /* malloc supplies at least max_align_t alignment, matching this target. */
    a20_rust_handle_table_state_t *state = malloc(state_size);
    uint8_t *entries = test_alloc(INITIAL_CAPACITY * entry_size);
    uint64_t *bitmap = test_alloc(sizeof(*bitmap));
    assert(state && entries && bitmap);
    assert(a20_rust_handle_table_state_init(
               state, entries, entry_size, _Alignof(test_handle_entry_t),
               INITIAL_CAPACITY, bitmap, 1,
               MAX_CAPACITY, MAX_CAPACITY, test_alloc, test_free) == 0);

    uint32_t slots[4] = { UINT32_MAX, UINT32_MAX, UINT32_MAX, UINT32_MAX };
    assert(a20_rust_handle_table_reserve_many(state, 2, slots) == 0);
    assert(slots[0] == 0 && slots[1] == 1);
    assert(a20_rust_handle_table_used(state) == 2);
    assert(a20_rust_handle_table_capacity(state) == INITIAL_CAPACITY);
    assert(a20_rust_handle_table_abort_many(state, slots, 1) == 1);
    assert(a20_rust_handle_table_install_at(
               state, slots[0], (void *)object_a, 7, 5, 0, 2, 2, 0) == slots[0]);
    assert(a20_rust_handle_table_used(state) == 2);

    /* First half of growth succeeds, second fails. State and old buffers stay
     * intact, and the speculative first allocation is returned exactly once. */
    const unsigned frees_before_failed_growth = free_calls;
    fail_call = alloc_calls + 2;
    assert(a20_rust_handle_table_reserve_many(state, 1, slots + 2) == -13);
    fail_call = 0;
    assert(a20_rust_handle_table_capacity(state) == INITIAL_CAPACITY);
    assert(a20_rust_handle_table_used(state) == 2);
    assert(free_calls == frees_before_failed_growth + 1);
    test_handle_entry_t snapshot = {0};
    assert(a20_rust_handle_table_lookup(state, slots[0], 7, 1, 0, &snapshot) == 0);
    assert(snapshot.object == (void *)object_a && snapshot.remaining_ops == 1);

    uint32_t abort_one[1] = { slots[1] };
    assert(a20_rust_handle_table_abort_many(state, abort_one, 1) == 1);
    assert(a20_rust_handle_table_reserve_many(state, 2, slots + 1) == 0);
    assert(a20_rust_handle_table_used(state) == 3);
    assert(a20_rust_handle_table_capacity(state) == 4);
    memset(&snapshot, 0, sizeof(snapshot));
    assert(a20_rust_handle_table_lookup(state, slots[0], 7, 1, 0, &snapshot) == 0);
    assert(snapshot.object == (void *)object_a && snapshot.remaining_ops == 0);

    /* Duplicate abort and quota overflow must not corrupt the count. */
    uint32_t duplicate_abort_slot[1];
    assert(a20_rust_handle_table_reserve_many(state, 1, duplicate_abort_slot) == 0);
    abort_one[0] = duplicate_abort_slot[0];
    assert(a20_rust_handle_table_abort_many(state, abort_one, 1) == 1);
    assert(a20_rust_handle_table_abort_many(state, abort_one, 1) == 0);
    assert(a20_rust_handle_table_used(state) == 3);
    assert(a20_rust_handle_table_reserve_many(state, 7, slots) == -13);
    assert(a20_rust_handle_table_used(state) == 3);

    const uint32_t abort_pair[] = { slots[1], slots[2] };
    assert(a20_rust_handle_table_abort_many(state, abort_pair, 2) == 2);
    assert(a20_rust_handle_table_used(state) == 1);
    detached = NULL;
    detached_type = 0;
    assert(a20_rust_handle_table_remove(
               state, slots[0], &detached, &detached_type) == 1);
    assert(detached == (void *)object_a && detached_type == 7);
    assert(a20_rust_handle_table_used(state) == 0);

    /* C ht_alloc_slot starts at the containing bitmap word, so fixed slot 7
     * leaves slot 0 as the next ordinary allocation in word zero. */
    assert(a20_rust_handle_table_install_at(
               state, 7, (void *)object_a, 7, 1, 0, 0, 0, 0) == 7);
    assert(a20_rust_handle_table_install_at(
               state, 7, (void *)object_b, 7, 1, 0, 0, 0, 0) == -10);
    assert(a20_rust_handle_table_reserve_many(state, 1, slots) == 0);
    assert(slots[0] == 0);
    const uint32_t abort_pair2[] = { 7, slots[0] };
    assert(a20_rust_handle_table_remove(
               state, 7, &detached, &detached_type) == 1);
    assert(detached == (void *)object_a && detached_type == 7);
    assert(a20_rust_handle_table_abort_many(state, abort_pair2 + 1, 1) == 1);
    assert(a20_rust_handle_table_used(state) == 0);

    /* The composite APIs must keep Entry state and bitmap/quota accounting in
     * one Rust transition. In particular install_at reserves first, then must
     * commit CLOSING -> ACTIVE. */
    assert(a20_rust_handle_table_install_at(
               state, 3, (void *)object_a, 7, 5, 0, 2, 2, 0) == 3);
    assert(a20_rust_handle_table_used(state) == 1);
    assert(a20_rust_handle_table_install_at(
               state, 3, (void *)object_b, 7, 5, 0, 0, 0, 0) == -10);
    memset(&snapshot, 0, sizeof(snapshot));
    assert(a20_rust_handle_table_lookup(
               state, 3, 7, 1, 0, &snapshot) == 0);
    assert(snapshot.object == (void *)object_a && snapshot.object_type == 7);
    assert(snapshot.remaining_ops == 1 && snapshot.state == 1);
    assert(a20_rust_handle_table_lookup(
               state, 3, 7, 1, 0, &snapshot) == 0);
    assert(snapshot.remaining_ops == 0);
    assert(a20_rust_handle_table_lookup(
               state, 3, 7, 1, 0, &snapshot) == -7);

    /* Non-autoclose expiry leaves an allocated unusable handle until remove. */
    assert(a20_rust_handle_table_install_at(
               state, 4, (void *)object_b, 8, 3, 5, 0, 1, 0) == 4);
    detached = NULL;
    detached_type = 0;
    assert(a20_rust_handle_table_expire(
               state, 4, 5, &detached, &detached_type) == 0);
    assert(a20_rust_handle_table_lookup(
               state, 4, 8, 0, 5, &snapshot) == -25);
    assert(a20_rust_handle_table_remove(
               state, 4, &detached, &detached_type) == 1);
    assert(detached == (void *)object_b && detached_type == 8);

    /* AUTOCLOSE expiry detaches and releases the slot exactly once. */
    assert(a20_rust_handle_table_install_at(
               state, 5, (void *)object_c, 9, 1, 9, 0, 5, 0) == 5);
    detached = NULL;
    detached_type = 0;
    assert(a20_rust_handle_table_expire(
               state, 5, 9, &detached, &detached_type) == 1);
    assert(detached == (void *)object_c && detached_type == 9);
    assert(a20_rust_handle_table_used(state) == 1);

    /* Commit and batch-abort also reconcile Entry state and counts. */
    uint32_t commit_slot[1];
    assert(a20_rust_handle_table_reserve_many(state, 1, commit_slot) == 0);
    assert(a20_rust_handle_table_commit(
               state, commit_slot[0], (void *)object_b, 8, 1, 0, 0, 0, 0) == 0);
    assert(a20_rust_handle_table_commit(
               state, commit_slot[0], (void *)object_c, 8, 1, 0, 0, 0, 0) == -12);
    uint32_t abort_slot[1];
    assert(a20_rust_handle_table_reserve_many(state, 1, abort_slot) == 0);
    const uint32_t abort_slots[] = { abort_slot[0], abort_slot[0] };
    assert(a20_rust_handle_table_abort_many(state, abort_slots, 2) == 1);
    assert(a20_rust_handle_table_used(state) == 2);

    /* Teardown must drain a mix of live entries and a reservation. */
    assert(a20_rust_handle_table_install_at(
               state, 1, (void *)object_c, 9, 1, 0, 0, 0, 0) == 1);
    uint32_t reserved_slot[1];
    assert(a20_rust_handle_table_reserve_many(state, 1, reserved_slot) == 0);
    uint32_t cursor = 0;
    unsigned detached_objects = 0;
    const uintptr_t no_object = 0xdead;
    int drained;
    do {
        detached = (void *)no_object;
        drained = a20_rust_handle_table_destroy_next(
            state, &cursor, &detached, &detached_type);
        if (drained == 1 && detached != (void *)no_object)
            detached_objects++;
    } while (drained == 1);
    assert(detached_objects == 3);
    assert(a20_rust_handle_table_used(state) == 0);
    assert(a20_rust_handle_table_state_fini(state) == 0);
    free(state);
    assert(live_allocations == 0);
    assert(successful_allocations == free_calls);
    puts("handle-table-state: PASS combined transitions, transactional growth, quota and teardown");
    return 0;
}

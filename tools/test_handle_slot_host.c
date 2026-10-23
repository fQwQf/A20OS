#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#include "ipc/handle_table.h"
#include "ipc/objstats.h"

typedef struct test_object {
    _Atomic int refs;
} test_object_t;

a20_objstats_t g_a20_objstats;
_Noreturn void panic(const char *fmt, ...) {
    (void)fmt;
    abort();
}
static _Atomic uint64_t g_now;
static _Atomic int g_fail_kmalloc_after = -1;
extern uint64_t a20_ht_sig_blocked(struct a20_ht_internal *ht);
extern uint64_t a20_ht_sig_take(struct a20_ht_internal *ht);
extern void a20_handle_host_set_quota(struct a20_ht_internal *ht,
                                      uint32_t quota);

void *kmalloc(size_t size) {
    int remaining = atomic_load(&g_fail_kmalloc_after);
    if (remaining == 0)
        return NULL;
    if (remaining > 0)
        atomic_fetch_sub(&g_fail_kmalloc_after, 1);
    void *memory = malloc(size);
    if (memory)
        memset(memory, 0xa5, size);
    return memory;
}
void kfree(void *ptr) {
    free(ptr);
}
uint64_t timer_get_ticks(void) {
    return atomic_load(&g_now);
}
void sched_note_timer_deadline(uint64_t tick) {
    (void)tick;
}
void a20_object_ref(void *obj, uint16_t type) {
    (void)type;
    atomic_fetch_add(&((test_object_t *)obj)->refs, 1);
}
void a20_object_release(void *obj, uint16_t type) {
    (void)type;
    int old = atomic_fetch_sub(&((test_object_t *)obj)->refs, 1);
    assert(old > 0);
}

typedef struct race_args {
    struct a20_ht_internal *ht;
    a20_handle_t handle;
    test_object_t *obj;
    int64_t commit_rc;
    int64_t remove_rc;
} race_args_t;

static void *race_commit(void *opaque) {
    race_args_t *a = opaque;
    a->commit_rc = a20_handle_commit_reserved_temporal(
        a->ht, a->handle, a->obj, A20_OBJ_TASK, A20_RIGHT_WAIT, 0, 0, 0, 0);
    return NULL;
}

static void *race_remove(void *opaque) {
    race_args_t *a = opaque;
    a->remove_rc = a20_handle_remove(a->ht, a->handle);
    return NULL;
}

static void test_slot_reuse_and_temporal_stats(struct a20_ht_internal *ht) {
    test_object_t fixed = {.refs = 1};
    assert(a20_handle_install_at_temporal(ht, 5, &fixed, A20_OBJ_TASK,
                                          A20_RIGHT_WAIT, 0, 0, 0, 0) == 5);
    assert(atomic_load(&g_a20_objstats.handles) == 1);
    assert(a20_handle_remove(ht, 5) == A20_OK);
    assert(atomic_load(&g_a20_objstats.handles) == 0);
    assert(atomic_load(&fixed.refs) == 0);
    a20_handle_t reused = UINT32_MAX;
    assert(a20_handle_reserve_many(ht, &reused, 1) == A20_OK);
    assert(reused == 0);
    a20_handle_abort_reserved(ht, &reused, 1);

    test_object_t temporal = {.refs = 1};
    int64_t h = a20_handle_install_temporal(
        ht, &temporal, A20_OBJ_TASK, A20_RIGHT_WAIT, 10, 0,
        A20_TEMPORAL_EXPIRY_ABSOLUTE | A20_TEMPORAL_AUTO_CLOSE, 0);
    assert(h >= 0);
    assert(atomic_load(&g_a20_objstats.handles) == 1);
    atomic_store(&g_now, 10);
    a20_temporal_sweep(ht);
    assert(atomic_load(&g_a20_objstats.handles) == 0);
    assert(atomic_load(&temporal.refs) == 0);
    a20_handle_entry_t snapshot;
    assert(a20_handle_lookup_internal(ht, (a20_handle_t)h, A20_OBJ_TASK,
                                      A20_RIGHT_WAIT, &snapshot) < 0);
}

static void
test_lookup_temporal_and_reference_contract(struct a20_ht_internal *ht) {
    test_object_t limited = {.refs = 1};
    int64_t handle =
        a20_handle_install_temporal(ht, &limited, A20_OBJ_TASK, A20_RIGHT_WAIT,
                                    0, 2, A20_TEMPORAL_OP_COUNT, 0);
    assert(handle >= 0);

    a20_handle_entry_t snapshot;
    assert(a20_handle_lookup_internal(ht, (a20_handle_t)handle, A20_OBJ_TASK,
                                      A20_RIGHT_SIGNAL,
                                      &snapshot) == -A20_ERR_ACCESS);
    uint64_t expiry = 0;
    uint32_t remaining = 0, flags = 0;
    assert(a20_handle_get_temporal(ht, (a20_handle_t)handle, &expiry,
                                   &remaining, &flags) == A20_OK);
    assert(remaining == 2 && flags == A20_TEMPORAL_OP_COUNT);

    assert(a20_handle_lookup_ref_internal(ht, (a20_handle_t)handle,
                                          A20_OBJ_TASK, A20_RIGHT_WAIT,
                                          &snapshot) == A20_OK);
    assert(snapshot.remaining_ops == 1);
    assert(atomic_load(&limited.refs) == 2);
    a20_object_release(snapshot.object, snapshot.type);
    assert(a20_handle_lookup_internal(ht, (a20_handle_t)handle, A20_OBJ_TASK,
                                      A20_RIGHT_WAIT, &snapshot) == A20_OK);
    assert(snapshot.remaining_ops == 0);
    assert(a20_handle_lookup_internal(ht, (a20_handle_t)handle, A20_OBJ_TASK,
                                      A20_RIGHT_WAIT,
                                      &snapshot) == -A20_ERR_ACCESS);
    assert(a20_handle_set_temporal(ht, (a20_handle_t)handle, 0, 3,
                                   A20_TEMPORAL_OP_COUNT) == -A20_ERR_ACCESS);

    a20_temporal_sweep(ht);
    assert(a20_handle_lookup_internal(ht, (a20_handle_t)handle, A20_OBJ_TASK, 0,
                                      &snapshot) == -A20_ERR_EXPIRED);
    assert(a20_handle_set_temporal(ht, (a20_handle_t)handle, 0, 0,
                                   A20_TEMPORAL_OP_COUNT) ==
           -A20_ERR_BAD_HANDLE);
    assert(a20_handle_remove(ht, (a20_handle_t)handle) == A20_OK);
    assert(atomic_load(&limited.refs) == 0);
}

static void
test_null_objects_rejected_consistently(struct a20_ht_internal *ht) {
    uint64_t handles_before = atomic_load(&g_a20_objstats.handles);
    assert(a20_handle_install(ht, NULL, A20_OBJ_TASK, A20_RIGHT_WAIT) ==
           -A20_ERR_INVALID_ARGUMENT);
    assert(a20_handle_install_temporal(ht, NULL, A20_OBJ_TASK, A20_RIGHT_WAIT,
                                       0, 0, 0,
                                       0) == -A20_ERR_INVALID_ARGUMENT);
    assert(a20_handle_install_at_temporal(ht, 3, NULL, A20_OBJ_TASK,
                                          A20_RIGHT_WAIT, 0, 0, 0,
                                          0) == -A20_ERR_INVALID_ARGUMENT);
    assert(atomic_load(&g_a20_objstats.handles) == handles_before);

    a20_handle_t reserved;
    assert(a20_handle_reserve_many(ht, &reserved, 1) == A20_OK);
    assert(a20_handle_commit_reserved_temporal(ht, reserved, NULL, A20_OBJ_TASK,
                                               A20_RIGHT_WAIT, 0, 0, 0,
                                               0) == -A20_ERR_INVALID_ARGUMENT);
    assert(atomic_load(&g_a20_objstats.handles) == handles_before);
    a20_handle_abort_reserved(ht, &reserved, 1);
}

static void test_reservation_quota_and_stats(struct a20_ht_internal *ht) {
    a20_handle_t *slots = calloc(6, sizeof(*slots));
    assert(slots);
    a20_handle_host_set_quota(ht, 4);
    uint64_t before = atomic_load(&g_a20_objstats.handles);
    assert(a20_handle_reserve_many(ht, slots, 4) == A20_OK);
    assert(atomic_load(&g_a20_objstats.handles) == before);
    assert(a20_handle_reserve_many(ht, &slots[4], 1) == -A20_ERR_NO_SPACE);
    a20_handle_abort_reserved(ht, slots, 2);
    assert(a20_handle_reserve_many(ht, &slots[4], 3) == -A20_ERR_NO_SPACE);
    assert(a20_handle_reserve_many(ht, &slots[4], 2) == A20_OK);
    a20_handle_abort_reserved(ht, &slots[4], 2);
    a20_handle_abort_reserved(ht, &slots[2], 2);
    a20_handle_host_set_quota(ht, A20_HT_DEFAULT_QUOTA);
    assert(a20_handle_reserve_many(ht, slots, 1) == A20_OK);
    test_object_t committed = {.refs = 1};
    assert(a20_handle_commit_reserved_temporal(ht, slots[0], &committed,
                                               A20_OBJ_TASK, A20_RIGHT_WAIT, 0,
                                               0, 0, 0) == A20_OK);
    assert(atomic_load(&g_a20_objstats.handles) == before + 1);
    test_object_t rejected_duplicate = {.refs = 1};
    assert(a20_handle_commit_reserved_temporal(
               ht, slots[0], &rejected_duplicate, A20_OBJ_TASK, A20_RIGHT_WAIT,
               0, 0, 0, 0) == -A20_ERR_BAD_HANDLE);
    assert(atomic_load(&g_a20_objstats.handles) == before + 1);
    assert(atomic_load(&rejected_duplicate.refs) == 1);
    assert(a20_handle_remove(ht, slots[0]) == A20_OK);
    assert(atomic_load(&g_a20_objstats.handles) == before);
    assert(atomic_load(&committed.refs) == 0);
    free(slots);
}

static void test_growth_failure_is_atomic(struct a20_ht_internal *ht) {
    a20_handle_t *slots = calloc(A20_HT_INITIAL_CAP + 1, sizeof(*slots));
    assert(slots);
    atomic_store(&g_fail_kmalloc_after, 0);
    assert(a20_handle_reserve_many(ht, slots, A20_HT_INITIAL_CAP + 1) ==
           -A20_ERR_NO_SPACE);
    /* Keep all allocations failing: a successful full-capacity reserve proves
     * the failed growth did not strand a slot or silently expand the table. */
    assert(a20_handle_reserve_many(ht, slots, A20_HT_INITIAL_CAP) == A20_OK);
    uint8_t seen[A20_HT_INITIAL_CAP] = {0};
    for (uint32_t i = 0; i < A20_HT_INITIAL_CAP; i++) {
        assert(slots[i] < A20_HT_INITIAL_CAP);
        assert(!seen[slots[i]]);
        seen[slots[i]] = 1;
    }
    a20_handle_abort_reserved(ht, slots, A20_HT_INITIAL_CAP);
    atomic_store(&g_fail_kmalloc_after, -1);
    a20_handle_t one = UINT32_MAX;
    assert(a20_handle_reserve_many(ht, &one, 1) == A20_OK);
    a20_handle_abort_reserved(ht, &one, 1);
    free(slots);
}

static void test_commit_remove_race(struct a20_ht_internal *ht) {
    for (unsigned i = 0; i < 64; i++) {
        a20_handle_t slot;
        test_object_t obj = {.refs = 1};
        assert(a20_handle_reserve_many(ht, &slot, 1) == A20_OK);
        race_args_t args = {.ht = ht,
                            .handle = slot,
                            .obj = &obj,
                            .commit_rc = -1,
                            .remove_rc = -1};
        pthread_t commit_thread, remove_thread;
        assert(pthread_create(&commit_thread, NULL, race_commit, &args) == 0);
        assert(pthread_create(&remove_thread, NULL, race_remove, &args) == 0);
        assert(pthread_join(commit_thread, NULL) == 0);
        assert(pthread_join(remove_thread, NULL) == 0);
        assert(args.commit_rc == A20_OK);
        assert(args.remove_rc == A20_OK ||
               args.remove_rc == -A20_ERR_BAD_HANDLE);
        if (args.remove_rc < 0)
            assert(a20_handle_remove(ht, slot) == A20_OK);
        assert(atomic_load(&obj.refs) == 0);
    }
}

static void test_final_table_destroy_releases_live_slots(void) {
    struct a20_ht_internal *ht = a20_ht_create();
    assert(ht);
    test_object_t live = {.refs = 1};
    test_object_t expired = {.refs = 1};
    int64_t live_handle =
        a20_handle_install(ht, &live, A20_OBJ_TASK, A20_RIGHT_WAIT);
    assert(live_handle >= 0);
    int64_t expired_handle = a20_handle_install_temporal(
        ht, &expired, A20_OBJ_TASK, A20_RIGHT_WAIT, 1, 0, 0, 0);
    assert(expired_handle >= 0);
    a20_handle_t reserved = UINT32_MAX;
    assert(a20_handle_reserve_many(ht, &reserved, 1) == A20_OK);
    a20_handle_entry_t live_snapshot;
    int64_t live_lookup =
        a20_handle_lookup_internal(ht, (a20_handle_t)live_handle, A20_OBJ_TASK,
                                   A20_RIGHT_WAIT, &live_snapshot);
    assert(live_lookup == A20_OK);
    assert(live_snapshot.object == &live);
    assert(atomic_load(&g_a20_objstats.handles) == 2);
    a20_ht_destroy(ht);
    assert(atomic_load(&live.refs) == 0);
    assert(atomic_load(&expired.refs) == 0);
    assert(atomic_load(&g_a20_objstats.handles) == 0);
}

int main(void) {
    struct a20_ht_internal *ht = a20_ht_create();
    assert(ht);
    /* Real kmalloc does not promise zero-filled storage. No signal or mask
     * from a previous process may appear in a newly allocated table. */
    assert(a20_ht_sig_blocked(ht) == 0);
    assert(a20_ht_sig_take(ht) == 0);
    test_slot_reuse_and_temporal_stats(ht);
    test_lookup_temporal_and_reference_contract(ht);
    test_null_objects_rejected_consistently(ht);
    test_reservation_quota_and_stats(ht);
    test_growth_failure_is_atomic(ht);
    test_commit_remove_race(ht);
    a20_ht_put_ref(ht);
    assert(atomic_load(&g_a20_objstats.handles) == 0);
    test_final_table_destroy_releases_live_slots();
    puts("HANDLE_SLOT_HOST: PASS fixed_reuse=1 quota=1 stats=1 temporal=1 "
         "refs=1 destroy=1 null_rejected=1 commit_remove_race=64");
    return 0;
}

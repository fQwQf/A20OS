#define CONFIG_64BIT 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "channel_recv_txn.h"

static a20_channel_ep_t endpoint;
static struct a20_ht_internal table;
static int objects[A20_CH_MAX_HANDLES];
static int object_refs[A20_CH_MAX_HANDLES];
static a20_ch_handle_info_t message_infos[A20_CH_MAX_HANDLES];
static uint8_t message_data[16];
static uint32_t queued_len, queued_handles;
static int queued, lock_held, begin_refs, aborts, donated;
static int reserve_fail, alloc_fail, finish_fail, commit_fail_at;
static int copy_fail_at, copy_calls, publish_fail, publish_calls;
static int reserved_count, active_count, allocs, frees, bad, commits, cases;
static int reserve_abort_calls, reserve_abort_locked_calls;
static int finish_shorten, finish_extra, reserve_abort_first;
static struct {
    int active;
    void *object;
    uint16_t type;
    uint64_t rights, expiry;
    uint32_t remaining, flags;
    uint8_t label;
} slots[A20_CH_MAX_HANDLES];

static void arm_info(uint32_t i)
{
    object_refs[i] = 1; /* the queued message owns one reference */
    message_infos[i].object = &objects[i];
    message_infos[i].type = (uint16_t)(10 + i);
    message_infos[i].transfer_rights = 0x100u + i;
    message_infos[i].expiry_tick = 700 + i;
    message_infos[i].remaining_ops = 20 + i;
    message_infos[i].temporal_flags = 2 + i;
    message_infos[i].security_label = (uint8_t)(i % 3);
}

static void reset_case(uint32_t data_len, uint32_t handles)
{
    memset(&endpoint, 0, sizeof(endpoint));
    memset(&table, 0, sizeof(table));
    memset(objects, 0, sizeof(objects));
    memset(object_refs, 0, sizeof(object_refs));
    memset(message_infos, 0, sizeof(message_infos));
    memset(message_data, 0x5a, sizeof(message_data));
    memset(slots, 0, sizeof(slots));
    queued_len = data_len;
    queued_handles = handles;
    queued = 1;
    lock_held = begin_refs = aborts = donated = 0;
    reserve_fail = alloc_fail = finish_fail = publish_fail = 0;
    commit_fail_at = copy_fail_at = -1;
    copy_calls = publish_calls = reserved_count = active_count = 0;
    finish_shorten = finish_extra = 0;
    reserve_abort_first = -1;
    allocs = frees = bad = commits = 0;
    reserve_abort_calls = reserve_abort_locked_calls = 0;
    cases++;
    for (uint32_t i = 0; i < handles; i++)
        arm_info(i);
}

int64_t a20_channel_recv_begin(a20_channel_ep_t *ep, uint32_t flags,
                               uint32_t *data_len, uint32_t *handle_count)
{
    (void)ep; (void)flags;
    if (!queued) return -18;
    lock_held = 1;
    begin_refs++;
    *data_len = queued_len;
    *handle_count = queued_handles;
    return 0;
}

int64_t a20_channel_recv_begin_donate(a20_channel_ep_t *ep, uint32_t flags,
                                      uint32_t *data_len, uint32_t *handle_count)
{
    donated++;
    return a20_channel_recv_begin(ep, flags, data_len, handle_count);
}

static void end_begin(void)
{
    if (!lock_held || begin_refs != 1) bad++;
    lock_held = 0;
    begin_refs--;
}

void a20_channel_recv_abort(a20_channel_ep_t *ep)
{
    (void)ep;
    end_begin();
    aborts++;
}

int64_t a20_channel_recv_finish(a20_channel_ep_t *ep, void *data,
                                uint32_t *data_len,
                                a20_ch_handle_info_t *handles,
                                uint32_t *handle_count)
{
    (void)ep;
    if (finish_fail) {
        end_begin();
        return -4;
    }
    /* finish_shorten models a sender that delivers fewer infos than begin
     * announced; finish_extra models a helper whose reported count exceeds
     * the reservation the caller made for it. Both are reachable only from a
     * broken internal contract, never from user input. */
    uint32_t announced = queued_handles - finish_shorten;
    if (*data_len < queued_len || *handle_count < announced) {
        end_begin();
        return -13;
    }
    end_begin();
    if (queued_len) memcpy(data, message_data, queued_len);
    uint32_t delivered = announced + finish_extra;
    for (uint32_t i = announced; i < delivered && i < A20_CH_MAX_HANDLES; i++)
        arm_info(i);
    if (delivered)
        memcpy(handles, message_infos, delivered * sizeof(*handles));
    *data_len = queued_len;
    *handle_count = delivered;
    queued = 0;
    return 0;
}

int64_t a20_handle_reserve_many(struct a20_ht_internal *ht, a20_handle_t *out,
                                uint32_t count)
{
    (void)ht;
    if (!lock_held || begin_refs != 1) bad++;
    if (reserve_fail) return -13;
    for (uint32_t i = 0; i < count; i++) out[i] = 100 + i;
    reserved_count += (int)count;
    return 0;
}

void a20_handle_abort_reserved(struct a20_ht_internal *ht, a20_handle_t *hs,
                               uint32_t count)
{
    (void)ht; (void)hs;
    reserve_abort_calls++;
    if ((lock_held && begin_refs != 1) || (!lock_held && begin_refs != 0))
        bad++;
    if (lock_held) reserve_abort_locked_calls++;
    if (count && hs && reserve_abort_first < 0)
        reserve_abort_first = (int)(*hs - 100);
    reserved_count -= (int)count;
    if (reserved_count < 0) bad++;
}

int64_t a20_handle_commit_reserved_temporal(struct a20_ht_internal *ht,
                                             a20_handle_t h, void *object,
                                             uint16_t type, a20_rights_t rights,
                                             uint64_t expiry, uint32_t remaining,
                                             uint32_t flags, uint8_t label)
{
    (void)ht;
    commits++;
    if (lock_held || begin_refs) bad++;
    uint32_t index = h - 100;
    if ((int)index == commit_fail_at) return -5;
    if (index >= A20_CH_MAX_HANDLES || slots[index].active) { bad++; return -5; }
    slots[index].active = 1;
    slots[index].object = object;
    slots[index].type = type;
    slots[index].rights = rights;
    slots[index].expiry = expiry;
    slots[index].remaining = remaining;
    slots[index].flags = flags;
    slots[index].label = label;
    active_count++;
    reserved_count--;
    return 0;
}

int64_t a20_handle_remove(struct a20_ht_internal *ht, a20_handle_t h)
{
    (void)ht;
    if (lock_held || begin_refs) bad++;
    uint32_t index = h - 100;
    if (index >= A20_CH_MAX_HANDLES || !slots[index].active) {
        bad++;
        return -5;
    }
    slots[index].active = 0;
    active_count--;
    a20_object_release(slots[index].object, slots[index].type);
    return 0;
}

void a20_object_release(void *object, uint16_t type)
{
    (void)type;
    if (lock_held || begin_refs) bad++;
    for (uint32_t i = 0; i < A20_CH_MAX_HANDLES; i++) {
        if (object == &objects[i]) {
            object_refs[i]--;
            if (object_refs[i] < 0) bad++;
            return;
        }
    }
    bad++;
}

void *kmalloc(size_t size)
{
    if (alloc_fail) return NULL;
    void *p = malloc(size ? size : 1);
    if (p) allocs++;
    return p;
}

void kfree(void *p)
{
    if (!p) return;
    frees++;
    free(p);
}

long copy_to_user(void *dst, const void *src, size_t size)
{
    if (lock_held || begin_refs) bad++;
    copy_calls++;
    if (copy_calls == copy_fail_at) {
        if (size) memcpy(dst, src, 1); /* model a partial usercopy */
        return -1;
    }
    if (size) memcpy(dst, src, size);
    return (long)size;
}

static int publish(void *context, uint32_t data_len, uint32_t handle_count)
{
    uint32_t *out = context;
    publish_calls++;
    /* Final syscall-argument copyout may fault/sleep; endpoint lock is gone. */
    if (lock_held || begin_refs) bad++;
    out[0] = data_len;
    out[1] = handle_count;
    return publish_fail ? -1 : 0;
}

typedef int64_t (*runner_fn)(a20_channel_ep_t *, struct a20_ht_internal *,
                             uint32_t, int, void *, uint32_t, void *, uint32_t,
                             void *, void *, a20_channel_recv_publish_fn);

static void check(int ok, const char *name)
{
    if (!ok || bad != 0) {
        fprintf(stderr, "FAIL %s (bad=%d)\n", name, bad);
        exit(1);
    }
}

static void run_suite(runner_fn run, const char *impl)
{
    cases = 0;
    uint8_t data_out[16] = {0};
    a20_handle_t handle_out[A20_CH_MAX_HANDLES] = {0};
    a20_rights_t rights_out[A20_CH_MAX_HANDLES] = {0};
    uint32_t published[2] = {0};

#define CALL(cap, hcap, rights, ctx) \
    run(&endpoint, &table, 0, 0, data_out, (cap), handle_out, (hcap), \
        (rights), (ctx), publish)

    reset_case(4, 2);
    check(CALL(16, 8, rights_out, published) == 4, "success:status");
    check(!queued && !lock_held && !begin_refs && !reserved_count &&
          active_count == 2 && object_refs[0] == 1 && object_refs[1] == 1 &&
          allocs == 1 && frees == 1 && commits == 2,
          "success:ownership");
    check(memcmp(data_out, message_data, 4) == 0 &&
          handle_out[0] == 100 && handle_out[1] == 101 &&
          rights_out[0] == 0x100 && rights_out[1] == 0x101 &&
          published[0] == 4 && published[1] == 2,
          "success:copyout");
    check(slots[1].expiry == 701 && slots[1].remaining == 21 &&
          slots[1].flags == 3 && slots[1].label == 1,
          "success:temporal-label");
    a20_handle_remove(&table, 100);
    a20_handle_remove(&table, 101);
    check(object_refs[0] == 0 && object_refs[1] == 0, "success:cleanup");
    check(active_count == 0 && allocs == frees && reserved_count == 0,
          "success:resources-returned");

    reset_case(4, 2);
    check(CALL(3, 8, rights_out, published) == -13 && queued &&
          aborts == 1 && reserved_count == 0 && object_refs[0] == 1 &&
          object_refs[1] == 1 && active_count == 0 && allocs == frees &&
          !lock_held && !begin_refs && reserve_abort_calls == 0,
          "short-buffer:preserves-message");

    reset_case(4, 2);
    reserve_fail = 1;
    check(CALL(16, 8, rights_out, published) == -13 && queued &&
          aborts == 1 && reserved_count == 0 && object_refs[0] == 1 &&
          object_refs[1] == 1 && active_count == 0 && allocs == frees &&
          !lock_held && !begin_refs && reserve_abort_calls == 0,
          "reserve-failure:preserves-message");

    reset_case(4, 2);
    alloc_fail = 1;
    check(CALL(16, 8, rights_out, published) == -6 && queued &&
          aborts == 1 && reserved_count == 0 && object_refs[0] == 1 &&
          object_refs[1] == 1 && active_count == 0 && allocs == frees &&
          !lock_held && !begin_refs && reserve_abort_calls == 1 &&
          reserve_abort_locked_calls == 1,
          "allocation-failure:preserves-message");

    reset_case(4, 2);
    finish_fail = 1;
    check(CALL(16, 8, rights_out, published) == -4 && queued &&
          !lock_held && !begin_refs && aborts == 0 && reserved_count == 0 &&
          object_refs[0] == 1 && object_refs[1] == 1 && active_count == 0 &&
          allocs == frees && reserve_abort_calls == 1 &&
          reserve_abort_locked_calls == 0,
          "finish-failure:preserves-message");

    for (int fail_at = 0; fail_at <= 1; fail_at++) {
        reset_case(4, 2);
        commit_fail_at = fail_at;
        check(CALL(16, 8, rights_out, published) == -5 && !queued &&
              active_count == 0 && reserved_count == 0 &&
              object_refs[0] == 0 && object_refs[1] == 0 &&
              !lock_held && !begin_refs && allocs == frees &&
              commits == fail_at + 1 && reserve_abort_calls == 1 &&
              reserve_abort_locked_calls == 0,
              fail_at == 0 ? "commit-failure:index-0-exact-rollback" :
                             "commit-failure:index-1-exact-rollback");
    }

    for (int fail_at = 1; fail_at <= 3; fail_at++) {
        reset_case(4, 2);
        copy_fail_at = fail_at;
        check(CALL(16, 8, rights_out, published) == -8 && !queued &&
              active_count == 0 && reserved_count == 0 &&
              object_refs[0] == 0 && object_refs[1] == 0 &&
              !lock_held && !begin_refs && allocs == frees && commits == 2 &&
              reserve_abort_calls == 0 && reserve_abort_locked_calls == 0,
              "copyout-failure:rollback");
    }

    reset_case(4, 2);
    publish_fail = 1;
    check(CALL(16, 8, rights_out, published) == -8 && !queued &&
          active_count == 0 && reserved_count == 0 &&
          object_refs[0] == 0 && object_refs[1] == 0 &&
          !lock_held && !begin_refs && allocs == frees && commits == 2 &&
          reserve_abort_calls == 0 && reserve_abort_locked_calls == 0,
          "final-args-copy-failure:rollback");

    reset_case(0, 0);
    check(CALL(0, 0, NULL, published) == 0 && !queued &&
          !allocs && !frees && publish_calls == 1,
          "zero-message:success");

    reset_case(4, 1);
    check(run(&endpoint, &table, 0, 1, data_out, 16, handle_out, 8,
              rights_out, published, publish) == 4 && donated == 1,
          "donated-reply:success");
    for (uint32_t i = 0; i < A20_CH_MAX_HANDLES; i++)
        if (slots[i].active) a20_handle_remove(&table, 100 + i);

    /* A message that fills the whole reservation array. */
    const uint32_t maxh = A20_CH_MAX_HANDLES;
    reset_case(4, maxh);
    check(CALL(16, 8, rights_out, published) == 4 && !queued &&
          active_count == (int)maxh && reserved_count == 0 &&
          commits == (int)maxh && copy_calls == 3 && publish_calls == 1 &&
          published[0] == 4 && published[1] == maxh &&
          handle_out[maxh - 1] == 100 + maxh - 1 &&
          rights_out[maxh - 1] == 0x100u + maxh - 1 &&
          object_refs[maxh - 1] == 1 &&
          slots[maxh - 1].expiry == 700 + maxh - 1 &&
          slots[maxh - 1].label == 1 &&
          !lock_held && !begin_refs && aborts == 0 && allocs == frees,
          "max-handles:full-width-transfer");
    for (uint32_t i = 0; i < maxh; i++)
        a20_handle_remove(&table, 100 + i);
    for (uint32_t i = 0; i < maxh; i++)
        if (object_refs[i]) bad++;
    check(active_count == 0 && allocs == frees && reserved_count == 0,
          "max-handles:cleanup");

    /* begin announces more handles than the reservation array can hold. */
    reset_case(0, 0);
    queued_handles = A20_CH_MAX_HANDLES + 1;
    check(CALL(0, 32, NULL, published) == -21 && queued && aborts == 1 &&
          reserved_count == 0 && active_count == 0 && allocs == 0 &&
          reserve_abort_calls == 0 && publish_calls == 0 &&
          !lock_held && !begin_refs,
          "over-max-handles:rejected-before-reservation");

    /* Fewer infos delivered than reserved: the surplus reservation must be
     * released, and must never be installed or disclosed to the receiver. */
    reset_case(4, 3);
    finish_shorten = 1;
    memset(handle_out, 0, sizeof(handle_out));
    memset(rights_out, 0, sizeof(rights_out));
    check(CALL(16, 8, rights_out, published) == 4 && !queued &&
          active_count == 2 && reserved_count == 0 && commits == 2 &&
          reserve_abort_calls == 1 && reserve_abort_first == 2 &&
          reserve_abort_locked_calls == 0 && copy_calls == 3 &&
          handle_out[0] == 100 && handle_out[1] == 101 &&
          handle_out[2] == 0 && rights_out[0] == 0x100 &&
          rights_out[1] == 0x101 && rights_out[2] == 0 &&
          published[0] == 4 && published[1] == 2 &&
          object_refs[0] == 1 && object_refs[1] == 1 &&
          object_refs[2] == 1 && !lock_held && !begin_refs && aborts == 0 &&
          allocs == frees,
          "reservation-trim:surplus-released-not-installed");
    a20_handle_remove(&table, 100);
    a20_handle_remove(&table, 101);
    check(object_refs[0] == 0 && object_refs[1] == 0 && object_refs[2] == 1,
          "reservation-trim:cleanup");

    /* More infos delivered than reserved: nothing may be installed and every
     * delivered reference must be released exactly once. */
    reset_case(4, 2);
    finish_extra = 1;
    check(CALL(16, 8, rights_out, published) == -21 && !queued &&
          active_count == 0 && reserved_count == 0 && commits == 0 &&
          reserve_abort_calls == 1 && reserve_abort_first == 0 &&
          reserve_abort_locked_calls == 0 && copy_calls == 0 &&
          publish_calls == 0 && object_refs[0] == 0 && object_refs[1] == 0 &&
          object_refs[2] == 0 && !lock_held && !begin_refs && aborts == 0 &&
          allocs == frees,
          "over-delivery:rejects-and-releases");

    /* A missing final-argument callback is a fault. The user arrays are
     * already written at that point, so rollback restores kernel ownership
     * only; it cannot un-copy them. */
    reset_case(4, 2);
    check(run(&endpoint, &table, 0, 0, data_out, 16, handle_out, 8,
              rights_out, published, NULL) == -8 && !queued &&
          active_count == 0 && reserved_count == 0 && commits == 2 &&
          object_refs[0] == 0 && object_refs[1] == 0 &&
          reserve_abort_calls == 0 && copy_calls == 3 && publish_calls == 0 &&
          !lock_held && !begin_refs && aborts == 0 && allocs == frees,
          "missing-publish:full-rollback");

    /* The rights array is optional even when handles are installed: exactly
     * the data and handle copies may happen. */
    reset_case(4, 2);
    check(CALL(16, 8, NULL, published) == 4 && !queued &&
          active_count == 2 && reserved_count == 0 && commits == 2 &&
          copy_calls == 2 && publish_calls == 1 &&
          published[0] == 4 && published[1] == 2 &&
          object_refs[0] == 1 && object_refs[1] == 1 &&
          !lock_held && !begin_refs && aborts == 0,
          "null-rights:copy-skipped");
    a20_handle_remove(&table, 100);
    a20_handle_remove(&table, 101);
    check(object_refs[0] == 0 && object_refs[1] == 0 && allocs == frees,
          "null-rights:cleanup");

    check(bad == 0, "internal-accounting");
    printf("IPC_RECV_TXN_HOST: %s PASS cases=%d failures=0 refs=exact slots=exact allocations=exact\n",
           impl, cases);
#undef CALL
}

int main(void)
{
    run_suite(a20_channel_recv_txn_c, "c");
    run_suite(a20_rust_channel_recv_txn, "rust");
    return 0;
}

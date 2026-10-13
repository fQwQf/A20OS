#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/include/uapi/a20/cluster_apps.h"
static unsigned starts, stops, releases;
static int poll_state, start_result;
static a20_object_id_t root;
static int start(void *ctx, uintptr_t image, uintptr_t *out,
                 a20_object_id_t *id) {
    (void)ctx;
    assert(image == 123);
    starts++;
    if (start_result < 0)
        return start_result;
    *out = 456;
    *id = root;
    return start_result;
}
static int poll(void *ctx, uintptr_t object, int32_t *code) {
    (void)ctx;
    assert(object == 456);
    *code = 37;
    return poll_state;
}
static int stop(void *ctx, uintptr_t object, uint32_t reason) {
    (void)ctx;
    assert(object == 456 && reason == 9);
    stops++;
    return 0;
}
static void release(void *ctx, uintptr_t object) {
    (void)ctx;
    assert(object == 456);
    releases++;
}
static a20_app_result_t call(a20_app_owner_t *o, a20_app_request_t *q) {
    uint8_t input[A20_APP_REQUEST], reply[A20_APP_REPLY];
    a20_app_result_t result;
    q->cookie++;
    assert(!a20_app_request_encode(input, sizeof(input), q));
    assert(a20_app_owner_dispatch(o, input, sizeof(input), reply,
                                  sizeof(reply)) == A20_APP_REPLY);
    assert(!a20_app_result_decode(reply, sizeof(reply), q, &result));
    return result;
}
static unsigned recycle_starts, recycle_releases;
static uintptr_t held_object;
static int held_exited;
static a20_object_id_t recycle_control;
static int recycle_start(void *ctx, uintptr_t image, uintptr_t *object,
                         a20_object_id_t *id) {
    (void)ctx;
    assert(image == 123);
    *object = 1000 + (++recycle_starts);
    *id = recycle_control;
    id->serial = *object;
    return 0;
}
static int recycle_poll(void *ctx, uintptr_t object, int32_t *code) {
    (void)ctx;
    *code = 37;
    return object == held_object && !held_exited ? 0 : 1;
}
static int recycle_stop(void *ctx, uintptr_t object, uint32_t reason) {
    (void)ctx;
    assert(object == held_object && reason == 9);
    held_exited = 1;
    return 0;
}
static void recycle_release(void *ctx, uintptr_t object) {
    (void)ctx;
    assert(object >= 1001);
    recycle_releases++;
}
static void recycling(void) {
    recycle_control = (a20_object_id_t){.instance_id = {1},
                                        .domain_id = {2},
                                        .boot_epoch = 3,
                                        .serial = 40,
                                        .kind = 1};
    a20_execution_info_t execution = {.size = 64,
                                      .version = 1,
                                      .elf_machine = 62,
                                      .pointer_bits = 64,
                                      .byte_order = 1,
                                      .calling_abi = 1,
                                      .native_major = 1,
                                      .features = 2,
                                      .memory_model = 2,
                                      .atomic_model = 1};
    a20_app_variant_t variant = {
        .manifest = {8}, .requirement = {62, 64, 1, 1, 1, 0, 2}, .image = 123};
    a20_app_record_t records[2];
    a20_app_owner_t o;
    assert(!a20_app_owner_init(&o, &recycle_control, &execution, &variant, 1,
                               records, 2, 0, recycle_start, recycle_poll,
                               recycle_stop, recycle_release));
    a20_app_request_t hello = {
        .op = A20_APP_HELLO, .version = 2, .manifest = {8}};
    a20_app_result_t v = call(&o, &hello);
    assert(v.token == 1 && !v.occupied && !v.active);
    a20_app_request_t held = {.op = A20_APP_START,
                              .version = 2,
                              .token = v.token,
                              .nonce = 1,
                              .controller = recycle_control,
                              .client = recycle_control,
                              .manifest = {8}};
    v = call(&o, &held);
    assert(!v.status && v.state == A20_APP_RUNNING);
    a20_object_id_t stable = v.root;
    held_object = records[0].object;
    held.op = A20_APP_RELEASE;
    assert(call(&o, &held).status == A20_APP_BUSY);
    a20_app_request_t retired = {0};
    for (unsigned i = 0; i < 32; i++) {
        v = call(&o, &hello);
        assert(v.token && v.active == 1 && v.occupied == 1);
        a20_app_request_t q = {.op = A20_APP_START,
                               .version = 2,
                               .token = v.token,
                               .nonce = 10 + i,
                               .controller = recycle_control,
                               .client = recycle_control,
                               .manifest = {8}};
        v = call(&o, &q);
        assert(!v.status && v.state == A20_APP_RUNNING);
        q.op = A20_APP_QUERY;
        v = call(&o, &q);
        assert(!v.status && v.state == A20_APP_EXITED && v.exit_code == 37);
        q.op = A20_APP_RELEASE;
        v = call(&o, &q);
        assert(!v.status && !v.state && v.occupied == 1 && v.active == 1);
        if (!i)
            retired = q;
        q.op = A20_APP_START;
        assert(call(&o, &q).status == A20_APP_STALE);
        held.op = A20_APP_QUERY;
        v = call(&o, &held);
        assert(!v.status && v.state == A20_APP_RUNNING &&
               a20_object_id_equal(&stable, &v.root));
    }
    assert(recycle_starts == 33 && recycle_releases == 32);
    retired.op = A20_APP_START;
    assert(call(&o, &retired).status == A20_APP_STALE);
    retired.op = A20_APP_RELEASE;
    assert(call(&o, &retired).status == A20_APP_STALE);
    /* Retire an unaccepted token: its late first START is also fenced. */
    v = call(&o, &hello);
    retired.token = v.token;
    retired.nonce = 100;
    assert(!call(&o, &retired).status);
    retired.op = A20_APP_START;
    assert(call(&o, &retired).status == A20_APP_STALE);
    assert(recycle_starts == 33);
    /* Competing clients share an offer; a loser cannot stop/retire the winner. */
    a20_app_request_t other = held;
    other.op = A20_APP_RELEASE;
    other.client.serial++;
    assert(call(&o, &other).status == A20_APP_CONFLICT);
    held.op = A20_APP_STOP;
    held.reason = 9;
    assert(!call(&o, &held).status);
    held.op = A20_APP_QUERY;
    held.reason = 0;
    assert(call(&o, &held).state == A20_APP_EXITED);
    held.op = A20_APP_RELEASE;
    assert(!call(&o, &held).status && recycle_releases == 33);
    /* A CA/1 record cannot be reclaimed using a CA/2 alias of its slot. */
    a20_app_request_t legacy = held;
    legacy.version = 1;
    legacy.token = 0;
    legacy.op = A20_APP_START;
    legacy.nonce = 200;
    assert(!call(&o, &legacy).status);
    other = legacy;
    other.version = 2;
    other.token = records[0].token;
    other.op = A20_APP_RELEASE;
    assert(call(&o, &other).status == A20_APP_CONFLICT);
    legacy.op = A20_APP_QUERY;
    assert(call(&o, &legacy).state == A20_APP_EXITED);
    assert(call(&o, &legacy).state == A20_APP_EXITED);
    /* Counter exhaustion disables reuse, never wraps to an old token. */
    recycle_control.serial++;
    held_object = 0;
    assert(!a20_app_owner_init(&o, &recycle_control, &execution, &variant, 1,
                               records, 1, 0, recycle_start, recycle_poll,
                               recycle_stop, recycle_release));
    o.next_token = UINT64_MAX;
    v = call(&o, &hello);
    other = (a20_app_request_t){.op = A20_APP_RELEASE,
                                .version = 2,
                                .token = v.token,
                                .nonce = 300,
                                .controller = recycle_control,
                                .client = recycle_control,
                                .manifest = {8}};
    assert(!call(&o, &other).status);
    v = call(&o, &hello);
    assert(v.token == UINT64_MAX);
    other.token = v.token;
    other.op = A20_APP_START;
    assert(!call(&o, &other).status);
    other.op = A20_APP_QUERY;
    assert(call(&o, &other).state == A20_APP_EXITED);
    other.op = A20_APP_RELEASE;
    assert(!call(&o, &other).status);
    v = call(&o, &hello);
    assert(!v.token && !v.active && !v.occupied && !o.next_token);
    other.op = A20_APP_START;
    assert(call(&o, &other).status == A20_APP_STALE);
    uint8_t bytes[A20_APP_REQUEST];
    assert(!a20_app_request_encode(bytes, sizeof(bytes), &other));
    a20_app_request_t saved = other, out = other;
    bytes[24] = 0;
    for (unsigned i = 25; i < 32; i++)
        bytes[i] = 0;
    assert(a20_app_request_decode(bytes, sizeof(bytes), &out) < 0 &&
           !memcmp(&saved, &out, sizeof(out)));
    puts("test_clx_apps: reusable token fencing, live preservation, 32 cycles, "
         "unaccepted cancellation, legacy isolation and no wrap PASS");
}
int main(void) {
    a20_object_id_t control = {.instance_id = {1},
                               .domain_id = {2},
                               .boot_epoch = 3,
                               .serial = 4,
                               .kind = 1};
    root = control;
    root.serial++;
    a20_execution_info_t execution = {.size = 64,
                                      .version = 1,
                                      .elf_machine = 62,
                                      .pointer_bits = 64,
                                      .byte_order = 1,
                                      .calling_abi = 1,
                                      .native_major = 1,
                                      .features = 7,
                                      .memory_model = 2,
                                      .atomic_model = 1};
    a20_app_variant_t variants[3] = {
        {.manifest = {8},
         .requirement = {183, 64, 1, 2, 1, 0, 2},
         .image = 999},
        {.manifest = {8}, .requirement = {62, 64, 1, 1, 1, 0, 2}, .image = 123},
        {.manifest = {9},
         .requirement = {62, 64, 1, 1, 1, 0, 2},
         .image = 123}};
    a20_app_record_t records[2];
    a20_app_owner_t o;
    assert(!a20_app_owner_init(&o, &control, &execution, variants, 3, records,
                               2, 0, start, poll, stop, release));
    a20_app_request_t q = {.op = 1};
    a20_app_result_t v = call(&o, &q);
    assert(!v.status && v.execution.elf_machine == 62);
    q = (a20_app_request_t){.op = 2,
                            .nonce = 1,
                            .controller = control,
                            .client = control,
                            .manifest = {8}};
    q.client.domain_id[0] = 7;
    q.reason = 2;
    assert(call(&o, &q).status == A20_APP_BUSY && starts == 0);
    q.reason = 1;
    v = call(&o, &q);
    assert(!v.status && v.state == 1 && starts == 1 &&
           a20_object_id_equal(&v.root, &root));
    v = call(&o, &q);
    assert(!v.status && starts == 1);
    q.manifest[0] = 9;
    assert(call(&o, &q).status == A20_APP_CONFLICT && starts == 1);
    q.manifest[0] = 8;
    q.reason = 0;
    poll_state = -1;
    q.op = 3;
    assert(call(&o, &q).state == A20_APP_LOST && !releases);
    poll_state = 0;
    assert(call(&o, &q).state == A20_APP_RUNNING);
    q.op = 4;
    q.reason = 9;
    assert(call(&o, &q).state == A20_APP_STOPPING && stops == 1);
    assert(call(&o, &q).state == A20_APP_STOPPING && stops == 1);
    poll_state = 1;
    q.op = 3;
    q.reason = 0;
    v = call(&o, &q);
    assert(v.state == A20_APP_EXITED && v.exit_code == 37 && releases == 1);
    q.op = 2;
    assert(call(&o, &q).state == A20_APP_EXITED && starts == 1);
    q.nonce = 2;
    start_result = -12;
    assert(call(&o, &q).state == A20_APP_FAILED && starts == 2);
    assert(call(&o, &q).exit_code == -12 && starts == 2);
    q.nonce = 3;
    assert(call(&o, &q).status == A20_APP_NO_SPACE && starts == 2);
    q.manifest[0] = 10;
    assert(call(&o, &q).status == A20_APP_NO_IMAGE && starts == 2);
    q.manifest[0] = 8;
    q.controller.serial++;
    assert(call(&o, &q).status == A20_APP_STALE && starts == 2);
    q.controller = control;
    /* Ambiguous matching variants cannot choose an arbitrary executable. */
    variants[0] = variants[1];
    q.nonce = 4;
    assert(call(&o, &q).status == A20_APP_CONFLICT && starts == 2);
    uint8_t wire[A20_APP_REQUEST];
    assert(!a20_app_request_encode(wire, sizeof(wire), &q));
    wire[24] = 1;
    a20_app_request_t saved = q, out = q;
    assert(a20_app_request_decode(wire, sizeof(wire), &out) < 0 &&
           !memcmp(&saved, &out, sizeof(out)));
    /* A start whose identity cannot be observed is never executed again. */
    variants[0].requirement.elf_machine = 183;
    assert(!a20_app_owner_init(&o, &control, &execution, variants, 3, records,
                               2, 0, start, poll, stop, release));
    poll_state = 0;
    start_result = 1;
    root = (a20_object_id_t){0};
    q.nonce = 5;
    v = call(&o, &q);
    assert(!v.status && v.state == A20_APP_LOST && starts == 3);
    assert(call(&o, &q).state == A20_APP_LOST && starts == 3);
    poll_state = 1;
    q.op = 3;
    assert(call(&o, &q).state == A20_APP_EXITED && releases == 2);
    recycling();
    puts("test_clx_apps: variant admission, at-most-once start, lost "
         "observation, stop/join, tombstones, capacity and incarnation fencing "
         "PASS");
    return 0;
}

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
    puts("test_clx_apps: variant admission, at-most-once start, lost "
         "observation, stop/join, tombstones, capacity and incarnation fencing "
         "PASS");
    return 0;
}

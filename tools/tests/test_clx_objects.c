#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/include/uapi/a20/cluster_objects.h"
static a20_handle_identity_t ids[4];
static unsigned released[4];
static int identify(void *ctx, uintptr_t object, a20_handle_identity_t *out) {
    (void)ctx;
    assert(object < 4);
    *out = ids[object];
    return 0;
}
static void release(void *ctx, uintptr_t object) {
    (void)ctx;
    assert(object < 4);
    released[object]++;
}
static a20_object_message_t call(a20_object_directory_t *d, unsigned owner,
                                 unsigned object, int attached,
                                 a20_object_message_t *q, uint64_t now) {
    uint8_t request[96], reply[96];
    a20_object_message_t result;
    assert(!a20_object_request_encode(request, 96, q));
    assert(a20_object_directory_dispatch(d, owner, object, attached, request,
                                         96, reply, 96, now) == 96);
    assert(!a20_object_reply_decode(reply, 96, q, &result));
    return result;
}
int main(void) {
    a20_runtime_info_t runtime = {.size = 64,
                                  .version = 1,
                                  .state = 1,
                                  .instance_id = {1},
                                  .domain_id = {2},
                                  .boot_epoch = 3};
    for (unsigned i = 0; i < 4; i++)
        ids[i] = (a20_handle_identity_t){.size = 80,
                                         .version = 1,
                                         .state = 1,
                                         .id = {.instance_id = {1},
                                                .domain_id = {2},
                                                .boot_epoch = 3,
                                                .serial = i + 1,
                                                .kind = 1}};
    a20_object_record_t records[2];
    a20_object_directory_t d;
    assert(!a20_object_directory_init(&d, &runtime, records, 2, identify,
                                      release, 0));
    a20_object_message_t q = {
        .op = 2, .value = 100, .cookie = 1, .id = ids[0].id};
    assert(call(&d, 0, 0, 1, &q, 10).value == A20_OBJECT_DENIED &&
           released[0] == 1);
    a20_object_message_t a = call(&d, 1, 0, 1, &q, 10);
    assert(!a.value && a.state == 1 && a.publication == 1 && !released[1]);
    assert(call(&d, 1, 0, 1, &q, 11).publication == a.publication &&
           released[0] == 2);
    assert(call(&d, 2, 0, 1, &q, 12).value == A20_OBJECT_CONFLICT &&
           released[0] == 3);
    q.op = 1;
    q.value = 0;
    q.publication = a.publication;
    q.cookie++;
    assert(call(&d, 0, 0, 0, &q, 13).state == 1);
    ids[0].state = 2;
    assert(call(&d, 0, 0, 0, &q, 14).state == 2);
    q.op = 3;
    q.value = 100;
    assert(call(&d, 2, 0, 0, &q, 15).value == A20_OBJECT_DENIED);
    assert(!call(&d, 1, 0, 0, &q, 15).value);
    q.op = 1;
    q.value = 0;
    assert(call(&d, 0, 0, 0, &q, 114).state == 2);
    assert(call(&d, 0, 0, 0, &q, 115).value == A20_OBJECT_NOT_FOUND &&
           released[0] == 4);
    q.op = 2;
    q.value = 100;
    q.publication = 0;
    a = call(&d, 1, 0, 1, &q, 116);
    assert(a.publication == 2);
    q.op = 1;
    q.value = 0;
    q.publication = 1;
    assert(call(&d, 0, 0, 0, &q, 117).value == A20_OBJECT_EXPIRED);
    q.publication = 2;
    q.op = 4;
    assert(call(&d, 2, 0, 0, &q, 118).value == A20_OBJECT_DENIED);
    assert(!call(&d, 1, 0, 0, &q, 118).value && released[0] == 5);
    q = (a20_object_message_t){
        .op = 2, .value = 100, .cookie = 99, .id = ids[0].id};
    q.id.boot_epoch++;
    assert(call(&d, 1, 0, 1, &q, 120).value == A20_OBJECT_STALE);
    q.id = ids[1].id;
    assert(call(&d, 1, 0, 1, &q, 121).value == A20_OBJECT_DENIED);
    q.id = ids[0].id;
    assert(!call(&d, 1, 0, 1, &q, 122).value);
    q.id = ids[1].id;
    assert(!call(&d, 1, 1, 1, &q, 122).value);
    q.id = ids[2].id;
    assert(call(&d, 1, 2, 1, &q, 122).value == A20_OBJECT_NO_SPACE &&
           released[2] == 1);
    a20_object_directory_disconnect(&d, 2);
    assert(records[0].publication);
    a20_object_directory_disconnect(&d, 1);
    assert(!records[0].publication && !records[1].publication);
    assert(call(&d, 1, 2, 1, &q, UINT64_MAX - 99).value == A20_OBJECT_INVALID &&
           released[2] == 2);
    d.serial = UINT64_MAX;
    assert(call(&d, 1, 2, 1, &q, 123).value == A20_OBJECT_NO_SPACE &&
           released[2] == 3);
    uint8_t wire[96], reply[96];
    assert(!a20_object_request_encode(wire, 96, &q));
    wire[24] = 1;
    assert(a20_object_directory_dispatch(&d, 1, 3, 1, wire, 96, reply, 96,
                                         123) < 0 &&
           released[3] == 1);
    wire[24] = 0;
    a20_object_message_t saved = q, out = q;
    wire[2] = 2;
    assert(a20_object_request_decode(wire, 96, &out) < 0 &&
           !memcmp(&saved, &out, sizeof(out)));
    puts("test_clx_objects: capability publication, lease boundary, retired "
         "identity, owner/revoke, fencing and bounded cleanup PASS");
    return 0;
}

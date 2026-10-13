#ifndef A20_NATIVE_OBJECT_CONTRACT_H
#define A20_NATIVE_OBJECT_CONTRACT_H
#include "liba20rt/a20_handle.h"
#include "liba20rt/a20_runtime.h"
#include "../../kernel/include/uapi/a20/cluster_objects.h"
/* Same production directory on actual MCU/server user instructions. The
 * caller lends the handle during the thread test; revoke consumes it once.
 * Identity/close are real Native calls, time values are explicit test input.
 * This in-process adapter does not claim an MCU channel/network service. */
static a20_object_directory_t native_objects;
static a20_object_record_t native_object_record[1];
static a20_object_id_t native_object_id;
static uint64_t native_object_publication;
static int native_object_close_error, native_object_releases;
static int native_object_identify(void *ctx, uintptr_t object,
                                  a20_handle_identity_t *out) {
    (void)ctx;
    return a20_hdl_identity((a20_handle_t)object, out) < 0 ? -1 : 0;
}
static void native_object_release(void *ctx, uintptr_t object) {
    (void)ctx;
    native_object_close_error = a20_hdl_close((a20_handle_t)object) < 0;
    native_object_releases++;
}
static int native_object_call(a20_handle_t handle, unsigned op, unsigned state,
                              uint64_t now) {
    a20_object_message_t q = {.op = op,
                              .value = op == A20_OBJECT_PUBLISH ? 100 : 0,
                              .cookie = now,
                              .publication = op == A20_OBJECT_PUBLISH
                                                 ? 0
                                                 : native_object_publication,
                              .id = native_object_id},
                         reply;
    uint8_t input[96], output[96];
    if (a20_object_request_encode(input, 96, &q) ||
        a20_object_directory_dispatch(
            &native_objects, op == A20_OBJECT_QUERY ? 0 : 1, handle,
            op == A20_OBJECT_PUBLISH, input, 96, output, 96, now) != 96 ||
        a20_object_reply_decode(output, 96, &q, &reply) || reply.value ||
        reply.state != state)
        return -1;
    if (op == A20_OBJECT_PUBLISH)
        native_object_publication = reply.publication;
    return 0;
}
static int native_object_begin(a20_handle_t handle) {
    a20_runtime_info_t runtime;
    a20_handle_identity_t identity;
    if (a20_runtime_info(&runtime) || a20_hdl_identity(handle, &identity) ||
        a20_object_directory_init(
            &native_objects, &runtime, native_object_record, 1,
            native_object_identify, native_object_release, 0))
        return -1;
    native_object_id = identity.id;
    native_object_releases = native_object_close_error = 0;
    return native_object_call(handle, A20_OBJECT_PUBLISH, A20_OBJECT_LIVE, 1) ||
                   native_object_call(handle, A20_OBJECT_QUERY, A20_OBJECT_LIVE,
                                      2)
               ? -1
               : 0;
}
static int native_object_finish(a20_handle_t handle) {
    return native_object_call(handle, A20_OBJECT_QUERY, A20_OBJECT_RETIRED,
                              3) ||
                   native_object_call(handle, A20_OBJECT_REVOKE, 0, 4) ||
                   native_object_releases != 1 || native_object_close_error
               ? -1
               : 0;
}
#endif

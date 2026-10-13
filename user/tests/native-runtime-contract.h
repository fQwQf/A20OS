#ifndef A20_NATIVE_RUNTIME_CONTRACT_H
#define A20_NATIVE_RUNTIME_CONTRACT_H
#include "liba20rt/a20_runtime.h"
#include "liba20rt/a20_execution.h"
/* Test fixture only, never a deployment identity or persistent epoch source. */
static a20_runtime_binding_t native_runtime_expected;
static int native_runtime_matches(void) {
    a20_runtime_info_t info;
    return a20_runtime_info(&info) == A20_OK && a20_runtime_info_valid(&info) &&
           info.state == A20_RUNTIME_BOUND &&
           info.boot_epoch == native_runtime_expected.boot_epoch &&
           a20_runtime_id_equal(info.instance_id,
                                native_runtime_expected.instance_id) &&
           a20_runtime_id_equal(info.domain_id,
                                native_runtime_expected.domain_id);
}
static int native_runtime_contract(void) {
    _Static_assert(sizeof(a20_runtime_binding_t) == 64, "runtime bind ABI");
    _Static_assert(sizeof(a20_runtime_info_t) == 64, "runtime info ABI");
    _Static_assert(sizeof(a20_object_id_t) == 64, "object ID ABI");
    _Static_assert(sizeof(a20_handle_identity_t) == 80, "identity result ABI");
    a20_runtime_info_t info;
    a20_execution_info_t execution;
    if (a20_runtime_info(&info) || !a20_runtime_info_valid(&info) ||
        a20_execution_info(&execution))
        return 1;
    native_runtime_expected =
        (a20_runtime_binding_t){.size = 64,
                                .version = 1,
                                .instance_id = {0xa2, 0x20, 1},
                                .domain_id = {0xa2, 0x20, 2},
                                .boot_epoch = 0x1020304050607080ULL};
    native_runtime_expected.domain_id[14] = (uint8_t)execution.elf_machine;
    native_runtime_expected.domain_id[15] = (uint8_t)execution.pointer_bits;
    if (a20_runtime_info(0) != -A20_ERR_FAULT ||
        a20_runtime_bind(0) != -A20_ERR_FAULT)
        return 2;
    if (a20_syscall6(A20_SYS_runtime_info, (uintptr_t)&info, 63, 0, 0, 0, 0) !=
        -A20_ERR_INVALID_ARGUMENT)
        return 3;
    a20_runtime_binding_t bad = native_runtime_expected;
    bad.boot_epoch = 0;
    if (a20_runtime_bind(&bad) != -A20_ERR_INVALID_ARGUMENT)
        return 4;
    bad = native_runtime_expected;
    bad.flags = 1;
    if (a20_runtime_bind(&bad) != -A20_ERR_INVALID_ARGUMENT)
        return 5;
    bad = native_runtime_expected;
    bad.reserved64 = 1;
    if (a20_runtime_bind(&bad) != -A20_ERR_INVALID_ARGUMENT)
        return 6;
    if (a20_runtime_bind(&native_runtime_expected) ||
        a20_runtime_bind(&native_runtime_expected) || !native_runtime_matches())
        return 7;
    bad = native_runtime_expected;
    bad.boot_epoch++;
    if (a20_runtime_bind(&bad) != -A20_ERR_BUSY || !native_runtime_matches())
        return 8;
    bad = native_runtime_expected;
    bad.domain_id[0]++;
    if (a20_runtime_bind(&bad) != -A20_ERR_BUSY || !native_runtime_matches())
        return 9;
    bad = native_runtime_expected;
    bad.instance_id[0]++;
    if (a20_runtime_bind(&bad) != -A20_ERR_BUSY || !native_runtime_matches())
        return 10;
    return 0;
}
static int native_runtime_identity(a20_handle_t handle,
                                   a20_handle_identity_t *out, unsigned state) {
    return a20_hdl_identity(handle, out) == A20_OK && out->size == 80 &&
           out->version == 1 && out->state == state && !out->reserved &&
           a20_object_id_valid(&out->id) &&
           out->id.boot_epoch == native_runtime_expected.boot_epoch &&
           a20_runtime_id_equal(out->id.instance_id,
                                native_runtime_expected.instance_id) &&
           a20_runtime_id_equal(out->id.domain_id,
                                native_runtime_expected.domain_id);
}
#endif

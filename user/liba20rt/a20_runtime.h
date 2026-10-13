#ifndef A20_SDK_RUNTIME_H
#define A20_SDK_RUNTIME_H
#include "a20_types.h"
#include "a20_syscall.h"
#include "../../kernel/include/uapi/a20/runtime.h"
static inline a20_status_t a20_runtime_info(a20_runtime_info_t *out) {
    return a20_syscall6(A20_SYS_runtime_info, (uint64_t)(uintptr_t)out,
                        sizeof(*out), 0, 0, 0, 0);
}
static inline a20_status_t
a20_runtime_bind(const a20_runtime_binding_t *binding) {
    return a20_syscall6(A20_SYS_runtime_bind, (uint64_t)(uintptr_t)binding,
                        sizeof(*binding), 0, 0, 0, 0);
}
static inline a20_status_t a20_hdl_identity(a20_handle_t handle,
                                            a20_handle_identity_t *out) {
    return a20_syscall6(A20_SYS_handle_identity, handle,
                        (uint64_t)(uintptr_t)out, sizeof(*out), 0, 0, 0);
}
#endif

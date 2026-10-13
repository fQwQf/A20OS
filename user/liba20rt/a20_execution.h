#ifndef A20_SDK_EXECUTION_H
#define A20_SDK_EXECUTION_H
#include "a20_syscall.h"
#include "../../kernel/include/uapi/a20/execution.h"
#include "../../kernel/include/uapi/a20/execution_domain.h"

static inline a20_status_t a20_execution_info(a20_execution_info_t *out) {
    return a20_syscall6(A20_SYS_execution_info, (uint64_t)(uintptr_t)out,
                        sizeof(*out), 0, 0, 0, 0);
}
static inline a20_status_t
a20_execution_domain_info(a20_execution_domain_info_t *out) {
    return a20_syscall6(A20_SYS_execution_domain_info, (uint64_t)(uintptr_t)out,
                        sizeof(*out), 0, 0, 0, 0);
}
#endif

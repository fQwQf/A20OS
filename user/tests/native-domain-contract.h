#ifndef A20_NATIVE_DOMAIN_CONTRACT_H
#define A20_NATIVE_DOMAIN_CONTRACT_H
#include "liba20rt/a20_execution.h"
/* Shared by real MCU SVC and server syscall tests. */
static int native_domain_contract(a20_execution_domain_info_t *info) {
    _Static_assert(sizeof(*info) == 64, "domain info layout");
    if (a20_execution_domain_info(info) != A20_OK ||
        !a20_execution_domain_valid(info) || info->state != A20_DOMAIN_READY)
        return 1;
    a20_execution_info_t execution;
    if (a20_execution_info(&execution) != A20_OK ||
        info->memory_model != execution.memory_model ||
        info->atomic_model != execution.atomic_model)
        return 2;
    uint32_t before = info->size;
    if (a20_syscall6(A20_SYS_execution_domain_info, (uintptr_t)info, 63, 0, 0,
                     0, 0) != -A20_ERR_INVALID_ARGUMENT ||
        info->size != before)
        return 3;
    if (a20_execution_domain_info((void *)0) != -A20_ERR_FAULT)
        return 4;
    return 0;
}
#endif

/*
 * Service registry protocol (docs/hybrid-kernel/02-mainstream-plan.md M3).
 *
 * svcman claims the well-known registry server endpoint; clients use the
 * service_registry handle from their start_info to look up services by
 * name.  Registered endpoints travel as channel-transferred handles, so
 * binding is a capability grant with zero copying.
 */
#ifndef _A20_REGISTRY_H
#define _A20_REGISTRY_H

#include "a20_types.h"
#include "a20_syscall.h"

#define A20_REG_OP_LOOKUP    1u
#define A20_REG_OP_REGISTER  2u

#define A20_REG_NAME_MAX     28u

typedef struct a20_reg_req {
    uint32_t op;
    char     name[A20_REG_NAME_MAX];
} a20_reg_req_t;

typedef struct a20_reg_reply {
    int64_t  status;    /* 0 = ok, <0 = not found / error */
} a20_reg_reply_t;

static inline a20_status_t a20_registry_claim(void)
{
    return a20_syscall6(A20_SYS_registry_claim, 0, 0, 0, 0, 0, 0);
}

#endif

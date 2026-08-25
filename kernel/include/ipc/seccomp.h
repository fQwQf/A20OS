#ifndef _IPC_SECCOMP_H
#define _IPC_SECCOMP_H

#include "core/trap.h"
#include "core/types.h"
#include "proc/proc.h"

/* Classic-BPF seccomp engine backing seccomp(2)/PR_SET_SECCOMP. */

#define SECCOMP_MODE_DISABLED   0
#define SECCOMP_MODE_STRICT     1
#define SECCOMP_MODE_FILTER     2

#define SECCOMP_SET_MODE_STRICT     0
#define SECCOMP_SET_MODE_FILTER     1
#define SECCOMP_GET_ACTION_AVAIL    2
#define SECCOMP_GET_NOTIF_SIZES     3

typedef struct {
    uint32_t nr;
    uint32_t arch;
    uint64_t instruction_pointer;
    uint64_t args[6];
} seccomp_data_t;

typedef struct {
    uint64_t id;
    uint32_t pid;
    uint32_t flags;
    seccomp_data_t data;
} seccomp_notif_wire_t;

typedef struct {
    uint64_t id;
    int32_t val;
    int32_t error;
    uint32_t flags;
} seccomp_notif_resp_wire_t;

#define SECCOMP_OPS 0
#define SECCOMP_USER_NOTIF 1

#define SECCOMP_RET_KILL_PROCESS 0x80000000U
#define SECCOMP_RET_KILL_THREAD  0x00000000U
#define SECCOMP_RET_TRAP         0x00030000U
#define SECCOMP_RET_ERRNO        0x00050000U
#define SECCOMP_RET_USER_NOTIF   0x7fc00000U
#define SECCOMP_RET_TRACE        0x7ff00000U
#define SECCOMP_RET_LOG          0x7ffc0000U
#define SECCOMP_RET_ALLOW        0x7fff0000U
#define SECCOMP_RET_ACTION_FULL  0xffff0000U
#define SECCOMP_RET_DATA         0x0000ffffU

int seccomp_set_strict(task_t *t);
int seccomp_install_filter(task_t *t, const void *ufprog);
int seccomp_get_mode(const task_t *t);
void seccomp_inherit(task_t *child, const task_t *parent);
void seccomp_release(task_t *t);

/* Returns SECCOMP_RET_* for the given invocation, honoring the chain in
 * newest-first order.  SECCOMP_RET_ALLOW when no filter matches. */
uint32_t seccomp_evaluate(const task_t *t, uint64_t nr, uint64_t ip,
                          const uint64_t args[6]);

/* Dispatch hook: returns 0 to proceed with the syscall; returns 1 when the
 * action was applied and *ret_out holds the value to hand userspace. */
int64_t seccomp_gate(trap_context_t *ctx, uint64_t nr, int64_t *ret_out);

#endif /* _IPC_SECCOMP_H */

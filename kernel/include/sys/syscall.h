#ifndef _SYSCALL_H
#define _SYSCALL_H

#include "core/types.h"
#include "core/trap.h"

#define SYSCALL_PROFILE_MAX 1024

typedef struct syscall_prof {
    uint64_t count;
    uint64_t cycles;
} syscall_prof_t;

extern syscall_prof_t sys_prof[SYSCALL_PROFILE_MAX];

void syscall_init(void);
void syscall_profile_reset(void);
int64_t syscall_dispatch(trap_context_t *ctx);

#endif /* _SYSCALL_H */

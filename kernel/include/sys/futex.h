#ifndef _SYS_FUTEX_H
#define _SYS_FUTEX_H

#include <stdint.h>

struct task_t;

int futex_wake_user(int *uaddr, int nr);
int futex_wait_user_ns(int *uaddr, int expected, uint64_t timeout_ns);
void exit_robust_list(struct task_t *t);

#endif /* _SYS_FUTEX_H */

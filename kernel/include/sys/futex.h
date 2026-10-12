#ifndef _SYS_FUTEX_H
#define _SYS_FUTEX_H

#include <stdint.h>

struct task_t;

int futex_wake_user(int *uaddr, int nr);
int futex_wait_user_ns(int *uaddr, int expected, uint64_t timeout_ns);
int futex_atomic_user32(uint32_t *uaddr, unsigned operation, uint32_t expected,
                        uint32_t desired, uint32_t *observed,
                        uint32_t *exchanged);
void exit_robust_list(struct task_t *t);

#endif /* _SYS_FUTEX_H */

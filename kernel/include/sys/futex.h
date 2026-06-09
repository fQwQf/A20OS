#ifndef _SYS_FUTEX_H
#define _SYS_FUTEX_H

struct task_t;

int futex_wake_user(int *uaddr, int nr);
void exit_robust_list(struct task_t *t);

#endif /* _SYS_FUTEX_H */

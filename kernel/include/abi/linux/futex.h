#ifndef _ABI_LINUX_FUTEX_H
#define _ABI_LINUX_FUTEX_H

#define FUTEX_WAIT         0
#define FUTEX_WAKE         1
#define FUTEX_REQUEUE      3
#define FUTEX_CMP_REQUEUE  4
#define FUTEX_WAIT_BITSET  9
#define FUTEX_WAKE_BITSET  10
#define FUTEX_CMD_MASK     0x7F
#define FUTEX_CLOCK_REALTIME 0x100
#define FUTEX_BITSET_MATCH_ANY 0xffffffffU

#define FUTEX_WAITERS    0x80000000U
#define FUTEX_OWNER_DIED 0x40000000U
#define FUTEX_TID_MASK   0x3fffffffU
#define ROBUST_LIST_LIMIT 2048

struct robust_list {
    uintptr_t next;
};

struct robust_list_head {
    struct robust_list list;
    uint64_t futex_offset;
    struct robust_list *list_op_pending;
};

struct task_t;
void exit_robust_list(struct task_t *t);

#endif /* _ABI_LINUX_FUTEX_H */

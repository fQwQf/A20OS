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

#endif /* _ABI_LINUX_FUTEX_H */

#ifndef A20_ABI_LINUX_MMSG_TRANSACTION_H
#define A20_ABI_LINUX_MMSG_TRANSACTION_H

#include "core/types.h"

/* Adapter boundary for the Rust mmsg state machine.  The adapter keeps
 * usercopy and socket/vfile ownership in C; Rust owns batch progress,
 * error precedence, flags and timeout decisions. */
typedef struct a20_linux_mmsg_ops {
    int (*copy_header)(void *ctx, void *user_mmsg, unsigned index,
                       void *header_out, size_t header_size);
    int (*copy_msg_len)(void *ctx, void *user_mmsg, unsigned index,
                        uint32_t msg_len);
    int64_t (*send_one)(void *ctx, void *header, int flags);
    int64_t (*recv_one)(void *ctx, void *header, int flags,
                        uint64_t deadline);
    int (*read_timeout)(void *ctx, const void *user_timeout,
                        int64_t *sec, int64_t *nsec);
    int (*write_timeout)(void *ctx, void *user_timeout,
                         int64_t sec, int64_t nsec);
    uint64_t (*now_ticks)(void *ctx);
    uint64_t (*ticks_per_sec)(void *ctx);
} a20_linux_mmsg_ops_t;

int64_t a20_linux_sendmmsg_transaction(void *ctx, void *user_mmsg,
                                        unsigned vlen, int flags,
                                        size_t header_size,
                                        const a20_linux_mmsg_ops_t *ops);
int64_t a20_linux_recvmmsg_transaction(void *ctx, void *user_mmsg,
                                         unsigned vlen, int flags,
                                         void *user_timeout,
                                         size_t header_size,
                                         const a20_linux_mmsg_ops_t *ops);

#endif

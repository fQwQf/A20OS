#ifndef _CLUSTER_OPERATOR_H
#define _CLUSTER_OPERATOR_H

/* Architecture-neutral built-in operator, shared by leaf firmware and
 * hosted service adapters. No allocation, scheduler or transport calls.
 * Input: LE u32 count, i32 a[count], i32 b[count]. Output: LE status + i64.
 * A service slot is a directory binding, not part of the operator format. */
#include "cluster/uart.h"
#include "uapi/a20/cluster_work.h"

#define A20_CLX_OPERATOR_DOT 1u
#define A20_CLX_OPERATOR_INVALID 12u
#define A20_CLX_DOT_REPLY_BYTES 12u

static inline int a20_clx_operator_dot(const uint8_t *input, uint32_t len,
                                       uint8_t *reply, uint32_t capacity) {
    if (!reply || capacity < A20_CLX_DOT_REPLY_BYTES)
        return -1;
    a20_work_request_t request = {.input = input, .input_len = len};
    uint32_t output_len = 8;
    uint32_t status = input
                          ? a20_work_dot(NULL, &request, reply + 4, &output_len)
                          : A20_WORK_INVALID;
    a20_clx_put_le32(reply, status ? A20_CLX_OPERATOR_INVALID : 0);
    if (status)
        a20_clx_put_le64(reply + 4, 0);
    return A20_CLX_DOT_REPLY_BYTES;
}

#endif

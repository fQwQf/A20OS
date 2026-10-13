#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../kernel/cluster/operator.h"

int main(void) {
    uint8_t request[28] = {0}, reply[12];
    a20_clx_put_le32(request, 3);
    const int32_t a[] = {1, -2, 3}, b[] = {4, 5, -6};
    for (unsigned i = 0; i < 3; i++) {
        a20_clx_put_le32(request + 4 + 4 * i, (uint32_t)a[i]);
        a20_clx_put_le32(request + 16 + 4 * i, (uint32_t)b[i]);
    }
    assert(a20_clx_operator_dot(request, sizeof(request), reply,
                                sizeof(reply)) == 12);
    assert(a20_clx_get_le32(reply) == 0);
    assert((int64_t)a20_clx_get_le64(reply + 4) == -24);
    assert(a20_clx_operator_dot(request, sizeof(request), reply, 11) < 0);
    a20_clx_put_le32(request, 0x20000000u);
    assert(a20_clx_operator_dot(request, 4, reply, sizeof(reply)) == 12);
    assert(a20_clx_get_le32(reply) == A20_CLX_OPERATOR_INVALID);
    a20_clx_put_le32(request, 0);
    assert(a20_clx_operator_dot(request, 4, reply, sizeof(reply)) == 12);
    assert(a20_clx_get_le32(reply) == 0 && a20_clx_get_le64(reply + 4) == 0);
    assert(a20_clx_operator_dot(NULL, 0, reply, sizeof(reply)) == 12);
    assert(a20_clx_get_le32(reply) == A20_CLX_OPERATOR_INVALID);
    puts("test_clx_operator: shared leaf/host dot format passed");
    return 0;
}

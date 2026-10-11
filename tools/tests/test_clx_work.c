#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "uapi/a20/cluster_work.h"

static unsigned executed;
static uint64_t now;
static int cancel;
static uint64_t clock_ms(void *ctx) {
    (void)ctx;
    return now;
}
static int canceled(void *ctx, const a20_work_request_t *r) {
    (void)ctx;
    (void)r;
    return cancel;
}
static uint32_t custom(void *ctx, const a20_work_request_t *r, uint8_t *out,
                       uint32_t *n) {
    (void)ctx;
    (void)r;
    executed++;
    now += 10;
    a20_work_put64(out, 42);
    *n = 8;
    return A20_WORK_OK;
}

int main(void) {
    a20_work_operator_t ops[] = {
        {A20_WORK_DOT, A20_WORK_REPLAY_SAFE, 140, 8, a20_work_dot, 0},
        {A20_WORK_SUM64, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 136, 8,
         a20_work_sum64, 0},
        {99, 0, 0, 8, custom, 0},
    };
    a20_work_agent_t agent = {.operators = ops,
                              .operator_count = 3,
                              .max_input = 140,
                              .max_output = 8,
                              .max_inflight = 1,
                              .max_budget_ms = 30000,
                              .clock_ms = clock_ms,
                              .canceled = canceled};
    assert(a20_work_agent_valid(&agent));
    uint8_t input[256] = {0}, reply[256], data[28] = {0};
    a20_work_request_t r = {.kind = A20_WORK_QUERY};
    int len = a20_work_encode(input, sizeof(input), &r);
    int n = a20_work_dispatch(&agent, input, len, reply, sizeof(reply));
    assert(n == 32 + 24 + 3 * 16);
    assert(a20_work_u32(reply + 20) == 0);
    assert(a20_work_u32(reply + 32) == A20_WORK_CAP_OPERATOR);
    assert(a20_work_u32(reply + 52) == 3);
    a20_work_put32(data, 3);
    int32_t av[] = {1, -2, 3}, bv[] = {4, 5, -6};
    for (unsigned i = 0; i < 3; i++) {
        a20_work_put32(data + 4 + i * 4, (uint32_t)av[i]);
        a20_work_put32(data + 16 + i * 4, (uint32_t)bv[i]);
    }
    r = (a20_work_request_t){.kind = A20_WORK_OPERATOR,
                             .job = 0x123456789abcdef0ULL,
                             .shard = 7,
                             .operator_id = A20_WORK_DOT,
                             .budget_ms = 100,
                             .input_len = 28,
                             .attempt = 2,
                             .input = data};
    len = a20_work_encode(input, sizeof(input), &r);
    n = a20_work_dispatch(&agent, input, len, reply, sizeof(reply));
    assert(n == 40 && a20_work_u32(reply + 20) == 0);
    assert((int64_t)a20_work_u64(reply + 32) == -24);
    assert(a20_work_u64(reply + 4) == r.job && a20_work_u32(reply + 12) == 7);
    assert(a20_work_u32(reply + 28) == 2);
    r.kind = A20_WORK_PROCESS;
    len = a20_work_encode(input, sizeof(input), &r);
    assert(a20_work_dispatch(&agent, input, len, reply, sizeof(reply)) == 32);
    assert(a20_work_u32(reply + 20) == A20_WORK_UNSUPPORTED);
    r.kind = A20_WORK_OPERATOR;
    r.operator_id = 99;
    r.input_len = 0;
    len = a20_work_encode(input, sizeof(input), &r);
    assert(a20_work_dispatch(&agent, input, len, reply, 39) == 32);
    assert(a20_work_u32(reply + 20) == A20_WORK_NO_SPACE && executed == 0);
    cancel = 1;
    a20_work_dispatch(&agent, input, len, reply, sizeof(reply));
    assert(a20_work_u32(reply + 20) == A20_WORK_CANCELED && executed == 0);
    cancel = 0;
    r.budget_ms = 5;
    len = a20_work_encode(input, sizeof(input), &r);
    a20_work_dispatch(&agent, input, len, reply, sizeof(reply));
    assert(a20_work_u32(reply + 20) == A20_WORK_DEADLINE && executed == 1);
    r.budget_ms = 0;
    len = a20_work_encode(input, sizeof(input), &r);
    a20_work_dispatch(&agent, input, len, reply, sizeof(reply));
    assert(a20_work_u32(reply + 20) == A20_WORK_DEADLINE && executed == 1);
    ops[2].id = 1;
    assert(!a20_work_agent_valid(&agent));
    ops[2].id = 99;
    /* Deterministic bounded malformed-input soak of the shipped decoder. */
    uint32_t seed = 7;
    for (unsigned i = 0; i < 100000; i++) {
        seed = seed * 1664525u + 1013904223u;
        unsigned size = seed % sizeof(input);
        for (unsigned j = 0; j < size; j++) {
            seed = seed * 1664525u + 1013904223u;
            input[j] = (uint8_t)(seed >> 16);
        }
        n = a20_work_dispatch(&agent, input, size, reply, sizeof(reply));
        assert(n >= 32 && n <= (int)sizeof(reply));
        assert(a20_work_u32(reply + 24) == (unsigned)n - 32);
    }
    assert(executed == 1);
    puts("test_clx_work: capabilities, correlation, limits, cancellation, "
         "deadlines and 100k malformed requests passed");
}

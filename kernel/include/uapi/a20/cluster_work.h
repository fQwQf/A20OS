#ifndef A20_CLUSTER_WORK_H
#define A20_CLUSTER_WORK_H

/* Transport-neutral work protocol and allocation-free agent. This header is
 * shared by firmware, kernel adapters and Native applications. All wire
 * integers are explicitly little endian; C structs never cross the wire. */
#include <stdint.h>

#define A20_WORK_MAGIC 0x5743u
#define A20_WORK_VERSION 1u
#define A20_WORK_HEADER 32u
#define A20_WORK_QUERY 1u
#define A20_WORK_OPERATOR 2u
#define A20_WORK_PROCESS 3u
#define A20_WORK_OK 0u
#define A20_WORK_INVALID 1u
#define A20_WORK_UNSUPPORTED 2u
#define A20_WORK_NO_SPACE 3u
#define A20_WORK_DEADLINE 4u
#define A20_WORK_CANCELED 5u
#define A20_WORK_FAILED 6u
#define A20_WORK_REPLAY_SAFE 1u
#define A20_WORK_ASSOCIATIVE 2u
#define A20_WORK_CAP_OPERATOR 1u
#define A20_WORK_CAP_PROCESS 2u
#define A20_WORK_DOT 1u
#define A20_WORK_SUM64 2u
#define A20_WORK_CAP_BYTES 24u
#define A20_WORK_DESCRIPTOR_BYTES 16u

static inline uint32_t a20_work_u32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static inline uint64_t a20_work_u64(const uint8_t *p) {
    return a20_work_u32(p) | ((uint64_t)a20_work_u32(p + 4) << 32);
}
static inline void a20_work_put32(uint8_t *p, uint32_t x) {
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(x >> (i * 8));
}
static inline void a20_work_put64(uint8_t *p, uint64_t x) {
    a20_work_put32(p, (uint32_t)x);
    a20_work_put32(p + 4, (uint32_t)(x >> 32));
}

typedef struct a20_work_request {
    uint8_t kind;
    uint64_t job;
    uint32_t shard, operator_id, budget_ms, input_len, attempt;
    const uint8_t *input;
} a20_work_request_t;

typedef struct a20_work_result {
    uint32_t status, output_len;
    const uint8_t *output;
} a20_work_result_t;

/* Reject delayed replies from another job, shard, operator or attempt. */
static inline int a20_work_result_decode(const uint8_t *p, uint32_t n,
                                         const a20_work_request_t *request,
                                         a20_work_result_t *result) {
    if (!p || !request || !result || n < A20_WORK_HEADER || p[0] != 'C' ||
        p[1] != 'W' || p[2] != A20_WORK_VERSION || p[3] != request->kind ||
        a20_work_u64(p + 4) != request->job ||
        a20_work_u32(p + 12) != request->shard ||
        a20_work_u32(p + 16) != request->operator_id ||
        a20_work_u32(p + 28) != request->attempt ||
        a20_work_u32(p + 24) != n - A20_WORK_HEADER ||
        a20_work_u32(p + 20) > A20_WORK_FAILED ||
        (a20_work_u32(p + 20) && n != A20_WORK_HEADER))
        return -1;
    result->status = a20_work_u32(p + 20);
    result->output_len = n - A20_WORK_HEADER;
    result->output = p + A20_WORK_HEADER;
    return 0;
}

static inline int a20_work_decode(const uint8_t *p, uint32_t n,
                                  a20_work_request_t *r) {
    if (!p || !r || n < A20_WORK_HEADER || p[0] != 'C' || p[1] != 'W' ||
        p[2] != A20_WORK_VERSION || a20_work_u32(p + 24) != n - A20_WORK_HEADER)
        return -1;
    r->kind = p[3];
    r->job = a20_work_u64(p + 4);
    r->shard = a20_work_u32(p + 12);
    r->operator_id = a20_work_u32(p + 16);
    r->budget_ms = a20_work_u32(p + 20);
    r->input_len = a20_work_u32(p + 24);
    r->attempt = a20_work_u32(p + 28);
    r->input = p + A20_WORK_HEADER;
    return 0;
}

/* Return the encoded request length, or -1 without executing any work. */
static inline int a20_work_encode(uint8_t *p, uint32_t capacity,
                                  const a20_work_request_t *r) {
    if (!p || !r || capacity < A20_WORK_HEADER ||
        r->input_len > capacity - A20_WORK_HEADER ||
        (r->input_len && !r->input))
        return -1;
    p[0] = 'C';
    p[1] = 'W';
    p[2] = A20_WORK_VERSION;
    p[3] = r->kind;
    a20_work_put64(p + 4, r->job);
    a20_work_put32(p + 12, r->shard);
    a20_work_put32(p + 16, r->operator_id);
    a20_work_put32(p + 20, r->budget_ms);
    a20_work_put32(p + 24, r->input_len);
    a20_work_put32(p + 28, r->attempt);
    for (uint32_t i = 0; i < r->input_len; i++)
        p[A20_WORK_HEADER + i] = r->input[i];
    return (int)(A20_WORK_HEADER + r->input_len);
}

typedef uint32_t (*a20_work_run_fn)(void *ctx, const a20_work_request_t *r,
                                    uint8_t *output, uint32_t *output_len);
typedef struct a20_work_operator {
    uint32_t id, flags, max_input, max_output;
    a20_work_run_fn run;
    void *ctx;
} a20_work_operator_t;
typedef struct a20_work_agent {
    const a20_work_operator_t *operators;
    uint32_t operator_count, max_input, max_output, max_inflight, max_budget_ms;
    a20_work_run_fn process;
    void *process_ctx;
    uint64_t (*clock_ms)(void *ctx);
    int (*canceled)(void *ctx, const a20_work_request_t *r);
    void *clock_ctx;
} a20_work_agent_t;

static inline int a20_work_agent_valid(const a20_work_agent_t *a) {
    if (!a || !a->max_inflight || !a->max_budget_ms ||
        (a->operator_count && !a->operators) || a->operator_count > 256)
        return 0;
    for (uint32_t i = 0; i < a->operator_count; i++) {
        const a20_work_operator_t *op = &a->operators[i];
        if (!op->id || !op->run || op->max_input > a->max_input ||
            op->max_output > a->max_output ||
            op->flags & ~(A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE))
            return 0;
        for (uint32_t j = 0; j < i; j++)
            if (a->operators[j].id == op->id)
                return 0;
    }
    return 1;
}

/* A single synchronous admission. The owner serializes calls or supplies
 * independent contexts, and reserves replay storage before invoking this.
 * Callbacks obey their advertised output limits and cooperate with deadlines.
 * There is no platform-independent preemption of arbitrary firmware code. */
static inline int a20_work_dispatch(const a20_work_agent_t *a,
                                    const uint8_t *input, uint32_t len,
                                    uint8_t *reply, uint32_t capacity) {
    if (!reply || capacity < A20_WORK_HEADER)
        return -1;
    a20_work_request_t r = {0};
    uint32_t status = A20_WORK_INVALID, n = 0;
    int decoded = a20_work_decode(input, len, &r) == 0;
    if (decoded && a20_work_agent_valid(a)) {
        if (r.kind == A20_WORK_QUERY) {
            if (!r.input_len && !r.operator_id) {
                uint32_t required =
                    A20_WORK_CAP_BYTES +
                    a->operator_count * A20_WORK_DESCRIPTOR_BYTES;
                status = A20_WORK_NO_SPACE;
                if (capacity - A20_WORK_HEADER >= required) {
                    uint8_t *out = reply + A20_WORK_HEADER;
                    a20_work_put32(
                        out, (a->operator_count ? A20_WORK_CAP_OPERATOR : 0) |
                                 (a->process ? A20_WORK_CAP_PROCESS : 0));
                    a20_work_put32(out + 4, a->max_input);
                    a20_work_put32(out + 8, a->max_output);
                    a20_work_put32(out + 12, a->max_inflight);
                    a20_work_put32(out + 16, a->max_budget_ms);
                    a20_work_put32(out + 20, a->operator_count);
                    for (uint32_t i = 0; i < a->operator_count; i++) {
                        const a20_work_operator_t *op = &a->operators[i];
                        uint8_t *d = out + A20_WORK_CAP_BYTES +
                                     i * A20_WORK_DESCRIPTOR_BYTES;
                        a20_work_put32(d, op->id);
                        a20_work_put32(d + 4, op->flags);
                        a20_work_put32(d + 8, op->max_input);
                        a20_work_put32(d + 12, op->max_output);
                    }
                    n = required;
                    status = A20_WORK_OK;
                }
            }
        } else if (r.kind == A20_WORK_OPERATOR || r.kind == A20_WORK_PROCESS) {
            if (!r.budget_ms) {
                status = A20_WORK_DEADLINE;
            } else if (r.budget_ms <= a->max_budget_ms &&
                       r.input_len <= a->max_input && r.job) {
                a20_work_run_fn run = a->process;
                void *ctx = a->process_ctx;
                uint32_t max_output = a->max_output;
                if (r.kind == A20_WORK_OPERATOR) {
                    run = 0;
                    for (uint32_t i = 0; i < a->operator_count; i++) {
                        const a20_work_operator_t *op = &a->operators[i];
                        if (op->id == r.operator_id) {
                            if (r.input_len <= op->max_input) {
                                run = op->run;
                                ctx = op->ctx;
                                max_output = op->max_output;
                            }
                            break;
                        }
                    }
                }
                status = A20_WORK_UNSUPPORTED;
                if (run) {
                    status = A20_WORK_NO_SPACE;
                    if (capacity - A20_WORK_HEADER >= max_output) {
                        uint64_t started =
                            a->clock_ms ? a->clock_ms(a->clock_ctx) : 0;
                        n = max_output;
                        if (a->canceled && a->canceled(a->clock_ctx, &r)) {
                            status = A20_WORK_CANCELED;
                        } else {
                            status = run(ctx, &r, reply + A20_WORK_HEADER, &n);
                            if (n > max_output || status > A20_WORK_FAILED)
                                status = A20_WORK_FAILED;
                            if (a->clock_ms &&
                                a->clock_ms(a->clock_ctx) - started >=
                                    r.budget_ms)
                                status = A20_WORK_DEADLINE;
                            if (a->canceled && a->canceled(a->clock_ctx, &r))
                                status = A20_WORK_CANCELED;
                        }
                    }
                }
            }
        } else {
            status = A20_WORK_UNSUPPORTED;
        }
    }
    if (status)
        n = 0;
    reply[0] = 'C';
    reply[1] = 'W';
    reply[2] = A20_WORK_VERSION;
    reply[3] = r.kind;
    a20_work_put64(reply + 4, r.job);
    a20_work_put32(reply + 12, r.shard);
    a20_work_put32(reply + 16, r.operator_id);
    a20_work_put32(reply + 20, status);
    a20_work_put32(reply + 24, n);
    a20_work_put32(reply + 28, r.attempt);
    return (int)(A20_WORK_HEADER + n);
}

/* DOT and SUM64 accumulate modulo 2^64; signed products fit in i64.
 * This fixes overflow semantics across 32-bit MCUs and 64-bit servers. */
static inline uint32_t a20_work_dot(void *ctx, const a20_work_request_t *r,
                                    uint8_t *out, uint32_t *n) {
    (void)ctx;
    if (*n < 8 || r->input_len < 4 || (r->input_len - 4) % 8 ||
        a20_work_u32(r->input) != (r->input_len - 4) / 8)
        return A20_WORK_INVALID;
    uint32_t count = a20_work_u32(r->input);
    uint64_t sum = 0;
    for (uint32_t i = 0; i < count; i++) {
        int64_t av = (int32_t)a20_work_u32(r->input + 4 + i * 4);
        int64_t bv = (int32_t)a20_work_u32(r->input + 4 + (count + i) * 4);
        sum += (uint64_t)(av * bv);
    }
    a20_work_put64(out, sum);
    *n = 8;
    return A20_WORK_OK;
}
static inline uint32_t a20_work_sum64(void *ctx, const a20_work_request_t *r,
                                      uint8_t *out, uint32_t *n) {
    (void)ctx;
    if (*n < 8 || r->input_len % 8)
        return A20_WORK_INVALID;
    uint64_t sum = 0;
    for (uint32_t i = 0; i < r->input_len; i += 8)
        sum += a20_work_u64(r->input + i);
    a20_work_put64(out, sum);
    *n = 8;
    return A20_WORK_OK;
}

#endif

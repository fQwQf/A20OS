#ifndef A20_CLUSTER_JOB_H
#define A20_CLUSTER_JOB_H
#include "cluster_work.h"

#define A20_JOB_WAITING 0u
#define A20_JOB_READY 1u
#define A20_JOB_RUNNING 2u
#define A20_JOB_DONE 3u
#define A20_JOB_FAILED 4u
#define A20_JOB_CANCELED 5u
#define A20_JOB_DEADLINE 6u
#define A20_JOB_MAX_INPUT 344u
#define A20_JOB_MAX_OUTPUT 168u
#define A20_JOB_MAX_TASKS 64u
#define A20_JOB_NO_CHILD UINT32_MAX

/* One owner drives this allocation-free state machine on every architecture.
 * Definitions are caller-owned and stay immutable until the job terminates.
 * Each reduction has at most two predecessors. Optional output storage and
 * input framing extend the original eight-byte SUM64 graph without a heap. */
typedef int (*a20_job_combine_fn)(void *ctx, const uint8_t *const parts[2],
                                  const uint32_t lengths[2], uint32_t count,
                                  uint8_t *out, uint32_t capacity);
typedef struct a20_job_target {
    uint8_t node_id[16];
    uint64_t boot_epoch;
    uint32_t slot, max_input, max_output, max_inflight, flags;
} a20_job_target_t;
typedef struct a20_job_task {
    uint32_t kind, operator_id, flags, input_len;
    const uint8_t *input;
    uint32_t children[2], child_count;
    uint32_t state, attempt, retryable, committed_attempt;
    uint64_t lease_until, result;
    a20_job_target_t target;
    uint8_t *output;
    uint32_t output_capacity, output_len;
    a20_job_combine_fn combine;
    void *combine_ctx;
} a20_job_task_t;
typedef struct a20_job {
    uint64_t id, deadline;
    uint32_t state, task_count, max_attempts, attempt_ms, completed, retries;
    a20_job_task_t *tasks;
} a20_job_t;

static inline int a20_job_node_equal(const uint8_t a[16], const uint8_t b[16]) {
    for (unsigned i = 0; i < 16; i++)
        if (a[i] != b[i])
            return 0;
    return 1;
}
static inline int a20_job_init(a20_job_t *j, uint64_t id, a20_job_task_t *tasks,
                               uint32_t count, uint64_t now, uint32_t budget_ms,
                               uint32_t attempt_ms, uint32_t max_attempts) {
    if (!j || !id || !tasks || !count || count > A20_JOB_MAX_TASKS ||
        !budget_ms || budget_ms > 30000 || !attempt_ms ||
        attempt_ms > budget_ms || !max_attempts || max_attempts > 8 ||
        now > UINT64_MAX - budget_ms)
        return -1;
    /* Validate the entire graph before touching any runtime state. */
    for (uint32_t i = 0; i < count; i++) {
        a20_job_task_t *t = &tasks[i];
        if ((t->kind != A20_WORK_OPERATOR && t->kind != A20_WORK_PROCESS) ||
            !t->operator_id || t->input_len > A20_JOB_MAX_INPUT ||
            (t->input_len && !t->input) || t->child_count > 2 ||
            t->output_capacity > A20_JOB_MAX_OUTPUT ||
            (!t->output && t->output_capacity) ||
            (t->combine && !t->child_count) ||
            (t->flags & ~(A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE)))
            return -1;
        if (t->child_count &&
            (t->kind != A20_WORK_OPERATOR ||
             (!t->combine && t->operator_id != A20_WORK_SUM64) ||
             t->input_len || !(t->flags & A20_WORK_ASSOCIATIVE)))
            return -1;
        for (uint32_t k = 0; k < t->child_count; k++)
            if (t->children[k] >= i || (k && t->children[0] == t->children[1]))
                return -1;
    }
    *j = (a20_job_t){.id = id,
                     .deadline = now + budget_ms,
                     .state = A20_JOB_RUNNING,
                     .task_count = count,
                     .max_attempts = max_attempts,
                     .attempt_ms = attempt_ms,
                     .tasks = tasks};
    for (uint32_t i = 0; i < count; i++) {
        a20_job_task_t *t = &tasks[i];
        t->state = t->child_count ? A20_JOB_WAITING : A20_JOB_READY;
        t->attempt = t->retryable = t->committed_attempt = 0;
        t->lease_until = t->result = 0;
        t->output_len = 0;
        t->target = (a20_job_target_t){0};
    }
    return 0;
}
static inline void a20_job_cancel(a20_job_t *j) {
    if (j && j->state == A20_JOB_RUNNING)
        j->state = A20_JOB_CANCELED;
}
/* An uncertain execution can move to a fresh attempt only when BOTH the job
 * policy and the selected operator advertise replay safety. Processes never
 * acquire this property merely because a scheduler wants to retry them. */
static inline int a20_job_attempt_failed(a20_job_t *j, uint32_t index) {
    if (!j || j->state != A20_JOB_RUNNING || index >= j->task_count)
        return -1;
    a20_job_task_t *t = &j->tasks[index];
    if (t->state != A20_JOB_RUNNING)
        return -1;
    if (t->retryable && t->attempt < j->max_attempts) {
        t->state = A20_JOB_READY;
        j->retries++;
        return 0;
    }
    t->state = A20_JOB_FAILED;
    j->state = A20_JOB_FAILED;
    return -1;
}
static inline void a20_job_tick(a20_job_t *j, uint64_t now) {
    if (!j || j->state != A20_JOB_RUNNING)
        return;
    if (now >= j->deadline) {
        j->state = A20_JOB_DEADLINE;
        return;
    }
    for (uint32_t i = 0; i < j->task_count; i++) {
        a20_job_task_t *t = &j->tasks[i];
        if (t->state == A20_JOB_RUNNING && now >= t->lease_until) {
            a20_job_attempt_failed(j, i);
            if (j->state != A20_JOB_RUNNING)
                return;
        }
        if (t->state == A20_JOB_WAITING) {
            uint32_t k = 0;
            while (k < t->child_count &&
                   j->tasks[t->children[k]].state == A20_JOB_DONE)
                k++;
            if (k == t->child_count)
                t->state = A20_JOB_READY;
        }
    }
}
static inline void a20_job_node_failed(a20_job_t *j, const uint8_t node[16],
                                       uint64_t epoch) {
    if (!j || !node || j->state != A20_JOB_RUNNING)
        return;
    for (uint32_t i = 0; i < j->task_count; i++) {
        a20_job_task_t *t = &j->tasks[i];
        if (t->state == A20_JOB_RUNNING && t->target.boot_epoch == epoch &&
            a20_job_node_equal(t->target.node_id, node)) {
            a20_job_attempt_failed(j, i);
            if (j->state != A20_JOB_RUNNING)
                return;
        }
    }
}
static inline uint32_t a20_job_input_len(const a20_job_task_t *t) {
    return t->combine       ? UINT32_MAX
           : t->child_count ? t->child_count * 8
                            : t->input_len;
}
/* Pure framing of already committed child outputs. Business reduction runs
 * at the selected agent; a custom framer must be deterministic and bounded. */
static inline int a20_job_prepare_input(const a20_job_t *j, uint32_t index,
                                        uint8_t *out, uint32_t capacity) {
    if (!j || index >= j->task_count || !out)
        return -1;
    const a20_job_task_t *t = &j->tasks[index];
    if (!t->child_count) {
        if (t->input_len > capacity)
            return -1;
        for (uint32_t i = 0; i < t->input_len; i++)
            out[i] = t->input[i];
        return (int)t->input_len;
    }
    const uint8_t *parts[2];
    uint32_t lengths[2];
    uint8_t scalar[2][8];
    for (uint32_t i = 0; i < t->child_count; i++) {
        const a20_job_task_t *child = &j->tasks[t->children[i]];
        if (child->state != A20_JOB_DONE)
            return -1;
        lengths[i] = child->output_len;
        a20_work_put64(scalar[i], child->result);
        parts[i] = child->output ? child->output : scalar[i];
    }
    if (t->combine) {
        int n = t->combine(t->combine_ctx, parts, lengths, t->child_count, out,
                           capacity);
        return n >= 0 && (uint32_t)n <= capacity ? n : -1;
    }
    if (capacity < t->child_count * 8)
        return -1;
    for (uint32_t i = 0; i < t->child_count; i++) {
        if (lengths[i] != 8)
            return -1;
        for (unsigned k = 0; k < 8; k++)
            out[i * 8 + k] = parts[i][k];
    }
    return (int)(t->child_count * 8);
}
static inline int a20_job_concat_parts(void *ctx, const uint8_t *const parts[2],
                                       const uint32_t lengths[2],
                                       uint32_t count, uint8_t *out,
                                       uint32_t capacity) {
    (void)ctx;
    unsigned used = 0;
    for (unsigned i = 0; i < count; i++) {
        if (capacity - used < 4 || lengths[i] > capacity - used - 4)
            return -1;
        a20_work_put32(out + used, lengths[i]);
        used += 4;
        for (unsigned k = 0; k < lengths[i]; k++)
            out[used + k] = parts[i][k];
        used += lengths[i];
    }
    return (int)used;
}
/* Admission accounts for all tasks currently assigned to a service. The
 * transport owner closes old attempt endpoints before reusing their slots. */
static inline int a20_job_begin(a20_job_t *j, uint32_t index,
                                const a20_job_target_t *target, uint64_t now,
                                uint8_t *wire, uint32_t capacity) {
    if (!j || !target || !wire || index >= j->task_count ||
        j->state != A20_JOB_RUNNING || now >= j->deadline)
        return -1;
    a20_job_task_t *t = &j->tasks[index];
    uint8_t prepared[A20_JOB_MAX_INPUT];
    int prepared_len =
        a20_job_prepare_input(j, index, prepared, sizeof(prepared));
    if (prepared_len < 0)
        return -1;
    uint32_t input_len = (uint32_t)prepared_len, inflight = 0;
    unsigned any = 0, all = 255;
    for (unsigned i = 0; i < 16; i++) {
        any |= target->node_id[i];
        all &= target->node_id[i];
    }
    if (t->state != A20_JOB_READY || !any || all == 255 ||
        !target->boot_epoch || !target->slot || !target->max_inflight ||
        input_len > target->max_input ||
        target->max_output < (t->output ? t->output_capacity : 8) ||
        capacity < A20_WORK_HEADER + input_len ||
        t->attempt >= j->max_attempts ||
        (t->child_count && !(target->flags & A20_WORK_ASSOCIATIVE)))
        return -1;
    for (uint32_t i = 0; i < j->task_count; i++) {
        a20_job_task_t *running = &j->tasks[i];
        if (running->state == A20_JOB_RUNNING &&
            running->target.boot_epoch == target->boot_epoch &&
            running->target.slot == target->slot &&
            a20_job_node_equal(running->target.node_id, target->node_id))
            inflight++;
    }
    if (inflight >= target->max_inflight)
        return -1;
    uint64_t remaining = j->deadline - now;
    uint32_t budget =
        remaining < j->attempt_ms ? (uint32_t)remaining : j->attempt_ms;
    a20_work_request_t request = {.kind = t->kind,
                                  .job = j->id,
                                  .shard = index,
                                  .operator_id = t->operator_id,
                                  .budget_ms = budget,
                                  .attempt = t->attempt + 1,
                                  .input_len = input_len,
                                  .input = prepared};
    int n = a20_work_encode(wire, capacity, &request);
    if (n < 0)
        return -1;
    t->attempt++;
    t->target = *target;
    t->retryable = t->kind == A20_WORK_OPERATOR &&
                   (t->flags & target->flags & A20_WORK_REPLAY_SAFE);
    t->state = A20_JOB_RUNNING;
    t->lease_until = now + budget;
    return n;
}
/* Return 0 for a newly committed result, 1 for stale/duplicate results, -1
 * for a current attempt's protocol/business failure. No reply commits twice. */
static inline int a20_job_complete(a20_job_t *j, uint32_t index,
                                   const uint8_t node[16], uint64_t epoch,
                                   const uint8_t *wire, uint32_t len,
                                   uint64_t now) {
    if (!j || !node || index >= j->task_count)
        return -1;
    a20_job_tick(j, now);
    a20_job_task_t *t = &j->tasks[index];
    if (j->state != A20_JOB_RUNNING || t->state != A20_JOB_RUNNING ||
        epoch != t->target.boot_epoch ||
        !a20_job_node_equal(node, t->target.node_id))
        return 1;
    /* A delayed attempt is not a malformed reply for the current attempt. */
    if (wire && len >= A20_WORK_HEADER &&
        (a20_work_u64(wire + 4) != j->id || a20_work_u32(wire + 12) != index ||
         a20_work_u32(wire + 28) != t->attempt))
        return 1;
    a20_work_request_t request = {.kind = t->kind,
                                  .job = j->id,
                                  .shard = index,
                                  .operator_id = t->operator_id,
                                  .attempt = t->attempt};
    a20_work_result_t result;
    if (a20_work_result_decode(wire, len, &request, &result) < 0 ||
        result.status ||
        (t->output ? result.output_len > t->output_capacity
                   : result.output_len != 8)) {
        t->state = A20_JOB_FAILED;
        j->state = A20_JOB_FAILED;
        return -1;
    }
    t->output_len = result.output_len;
    if (t->output)
        for (uint32_t i = 0; i < result.output_len; i++)
            t->output[i] = result.output[i];
    t->result = result.output_len == 8 ? a20_work_u64(result.output) : 0;
    t->committed_attempt = t->attempt;
    t->state = A20_JOB_DONE;
    if (++j->completed == j->task_count)
        j->state = A20_JOB_DONE;
    return 0;
}

/* CJ/1 convenience job: split a DOT into <=16-element leaf operations,
 * followed by a binary SUM64 tree. All graph storage belongs to the caller. */
#define A20_JOB_WIRE_HEADER 32u
#define A20_JOB_DOT 1u
#define A20_JOB_MAX_ELEMENTS 128u
#define A20_JOB_DOT_LEAVES 8u
#define A20_JOB_DOT_TASKS 15u
static inline int a20_job_dot_graph(a20_job_task_t *tasks, uint32_t capacity,
                                    uint8_t inputs[A20_JOB_DOT_LEAVES][132],
                                    const uint8_t *vectors, uint32_t count) {
    if (!tasks || !inputs || (count && !vectors) ||
        count > A20_JOB_MAX_ELEMENTS)
        return -1;
    uint32_t leaves = count ? (count + 15) / 16 : 1;
    if (capacity < leaves * 2 - 1)
        return -1;
    for (uint32_t i = 0; i < leaves; i++) {
        uint32_t start = i * 16, n = count > start ? count - start : 0;
        if (n > 16)
            n = 16;
        a20_work_put32(inputs[i], n);
        for (uint32_t k = 0; k < n * 4; k++) {
            inputs[i][4 + k] = vectors[start * 4 + k];
            inputs[i][4 + n * 4 + k] = vectors[(count + start) * 4 + k];
        }
        tasks[i] = (a20_job_task_t){.kind = A20_WORK_OPERATOR,
                                    .operator_id = A20_WORK_DOT,
                                    .flags = A20_WORK_REPLAY_SAFE,
                                    .input = inputs[i],
                                    .input_len = 4 + n * 8};
    }
    uint32_t level[A20_JOB_DOT_LEAVES], width = leaves, next = leaves;
    for (uint32_t i = 0; i < leaves; i++)
        level[i] = i;
    while (width > 1) {
        uint32_t out = 0;
        for (uint32_t i = 0; i < width; i += 2) {
            if (i + 1 == width) {
                level[out++] = level[i];
                continue;
            }
            tasks[next] = (a20_job_task_t){
                .kind = A20_WORK_OPERATOR,
                .operator_id = A20_WORK_SUM64,
                .flags = A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE,
                .child_count = 2,
                .children = {level[i], level[i + 1]}};
            level[out++] = next++;
        }
        width = out;
    }
    return (int)next;
}
#endif

#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uapi/a20/cluster_word_job.h"
static uint32_t echo(void *ctx, const a20_work_request_t *r, uint8_t *out,
                     uint32_t *n) {
    (void)ctx;
    if (r->input_len > *n)
        return A20_WORK_NO_SPACE;
    memcpy(out, r->input, r->input_len);
    *n = r->input_len;
    return 0;
}
static const a20_work_operator_t operators[] = {
    {A20_WORK_WORD_MAP, A20_WORK_REPLAY_SAFE, 1024, A20_WORD_RESULT,
     a20_work_word_map, 0},
    {A20_WORK_WORD_MERGE, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 344,
     A20_WORD_RESULT, a20_work_word_merge, 0},
    {A20_WORK_CONCAT, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 344, 168,
     a20_work_concat, 0},
    {99, A20_WORK_REPLAY_SAFE, 168, 168, echo, 0}};
static const a20_work_agent_t agent = {.operators = operators,
                                       .operator_count = 4,
                                       .max_input = 1024,
                                       .max_output = 168,
                                       .max_inflight = 1,
                                       .max_budget_ms = 30000};
static a20_job_task_t tasks[A20_JOB_WORD_TASKS];
static uint8_t inputs[A20_JOB_WORD_LEAVES][A20_JOB_WORD_CHUNK],
    outputs[A20_JOB_WORD_TASKS][A20_WORD_RESULT];
static uint8_t wire[A20_WORK_HEADER + A20_JOB_MAX_INPUT],
    reply[A20_WORK_HEADER + A20_JOB_MAX_OUTPUT];
static a20_job_target_t target(unsigned id, unsigned max_input) {
    a20_job_target_t t = {.boot_epoch = 100 + id,
                          .slot = 2,
                          .max_input = max_input,
                          .max_output = 168,
                          .max_inflight = 1,
                          .flags = A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE};
    t.node_id[15] = id;
    return t;
}
static void execute(a20_job_t *job) {
    for (unsigned now = 4; job->state == A20_JOB_RUNNING; now++) {
        assert(now < 1000);
        a20_job_tick(job, now);
        for (unsigned i = 0; i < job->task_count; i++)
            if (tasks[i].state == A20_JOB_READY) {
                uint8_t prepared[344];
                int input_len =
                    a20_job_prepare_input(job, i, prepared, sizeof(prepared));
                assert(input_len >= 0);
                if (input_len > 140) {
                    a20_job_target_t leaf = target(2, 140);
                    assert(a20_job_begin(job, i, &leaf, now, wire,
                                         sizeof(wire)) < 0);
                    assert(!tasks[i].attempt);
                }
                a20_job_target_t t = target(input_len > 140 ? 1 : 2,
                                            input_len > 140 ? 344 : 140);
                int n = a20_job_begin(job, i, &t, now, wire, sizeof(wire));
                assert(n > 0);
                n = a20_work_dispatch(&agent, wire, n, reply, sizeof(reply));
                assert(n >= 32);
                assert(!a20_job_complete(job, i, t.node_id, t.boot_epoch, reply,
                                         n, now));
            }
    }
    assert(job->state == A20_JOB_DONE);
}
int main(void) {
    const char *sentence = "Alpha beta gamma DELTA alpha beta gamma delta ";
    uint8_t text[A20_JOB_MAX_TEXT], reference[132];
    for (unsigned i = 0; i < sizeof(text); i++)
        text[i] = sentence[i % strlen(sentence)];
    for (unsigned len = 0; len <= sizeof(text); len++) {
        int count = a20_job_word_graph(tasks, A20_JOB_WORD_TASKS, inputs,
                                       outputs, text, len);
        assert(count > 0 && count <= (int)A20_JOB_WORD_TASKS);
        a20_job_t job;
        assert(!a20_job_init(&job, len + 1, tasks, count, 0, 1000, 100, 3));
        execute(&job);
        a20_work_request_t q = {.input = text, .input_len = len};
        uint32_t n = sizeof(reference);
        assert(!a20_work_word_map(0, &q, reference, &n));
        assert(tasks[count - 1].output_len == n &&
               !memcmp(tasks[count - 1].output, reference, n));
        assert(a20_word_dictionary_valid(reference, n));
    }
    /* Twelve-byte words force five-word shards: exactly 16 leaves and a
     * 31-node tree, including all four reduction levels. */
    const char *word = "alpha1234567 ";
    for (unsigned i = 0; i < 1014; i++)
        text[i] = word[i % 13];
    int wide_count = a20_job_word_graph(tasks, A20_JOB_WORD_TASKS, inputs,
                                        outputs, text, 1014);
    assert(wide_count == (int)A20_JOB_WORD_TASKS);
    a20_job_t wide;
    assert(!a20_job_init(&wide, 9000, tasks, wide_count, 0, 1000, 100, 3));
    execute(&wide);
    assert(tasks[wide_count - 1].output_len == 20 &&
           a20_work_u32(tasks[wide_count - 1].output + 16) == 78);
    /* Generic byte results include zero bytes, ordered CONCAT and stale-attempt
     * isolation; the old result buffer is never committed into a new attempt. */
    uint8_t raw[3][168];
    tasks[0] = (a20_job_task_t){.kind = A20_WORK_OPERATOR,
                                .operator_id = 99,
                                .flags = A20_WORK_REPLAY_SAFE,
                                .input = (const uint8_t *)"left",
                                .input_len = 4,
                                .output = raw[0],
                                .output_capacity = 168};
    tasks[1] = (a20_job_task_t){.kind = A20_WORK_OPERATOR,
                                .operator_id = 99,
                                .flags = A20_WORK_REPLAY_SAFE,
                                .output = raw[1],
                                .output_capacity = 168};
    tasks[2] =
        (a20_job_task_t){.kind = A20_WORK_OPERATOR,
                         .operator_id = A20_WORK_CONCAT,
                         .flags = A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE,
                         .children = {0, 1},
                         .child_count = 2,
                         .output = raw[2],
                         .output_capacity = 168,
                         .combine = a20_job_concat_parts};
    a20_job_t job;
    assert(!a20_job_init(&job, 1000, tasks, 3, 0, 1000, 100, 3));
    a20_job_target_t t = target(2, 344);
    int n = a20_job_begin(&job, 0, &t, 1, wire, sizeof(wire));
    assert(n > 0);
    n = a20_work_dispatch(&agent, wire, n, reply, sizeof(reply));
    uint8_t old[200];
    memcpy(old, reply, n);
    a20_job_node_failed(&job, t.node_id, t.boot_epoch);
    assert(a20_job_begin(&job, 0, &t, 2, wire, sizeof(wire)) > 0);
    assert(a20_job_complete(&job, 0, t.node_id, t.boot_epoch, old, n, 3) == 1 &&
           !tasks[0].output_len);
    n = a20_work_dispatch(&agent, wire, 36, reply, sizeof(reply));
    assert(!a20_job_complete(&job, 0, t.node_id, t.boot_epoch, reply, n, 3));
    execute(&job);
    assert(!tasks[1].output_len && tasks[2].output_len == 4 &&
           !memcmp(raw[2], "left", 4));
    tasks[0].output_capacity = 3;
    memset(raw[0], 0x5a, sizeof(raw[0]));
    assert(!a20_job_init(&job, 1001, tasks, 1, 0, 1000, 100, 3));
    n = a20_job_begin(&job, 0, &t, 1, wire, sizeof(wire));
    assert(n > 0);
    n = a20_work_dispatch(&agent, wire, n, reply, sizeof(reply));
    assert(a20_job_complete(&job, 0, t.node_id, t.boot_epoch, reply, n, 2) < 0);
    assert(raw[0][0] == 0x5a && !tasks[0].output_len);
    /* Capacity and malformed canonical dictionaries fail explicitly. */
    a20_work_request_t q = {.input = (const uint8_t *)"a b c d e f g h i",
                            .input_len = 17};
    uint32_t out = 132;
    assert(a20_work_word_map(0, &q, reference, &out) == A20_WORK_NO_SPACE);
    q.input = (const uint8_t *)"abcdefghijklX";
    q.input_len = 13;
    out = 132;
    assert(a20_work_word_map(0, &q, reference, &out) == A20_WORK_NO_SPACE);
    q.input = (const uint8_t *)"one";
    q.input_len = 3;
    out = 132;
    assert(!a20_work_word_map(0, &q, reference, &out) && out == 20);
    uint8_t merge[48];
    a20_work_put32(reference + 16, UINT32_MAX);
    a20_work_put32(merge, 20);
    memcpy(merge + 4, reference, 20);
    a20_work_put32(merge + 24, 20);
    memcpy(merge + 28, reference, 20);
    q.input = merge;
    q.input_len = 48;
    out = 132;
    assert(a20_work_word_merge(0, &q, reference, &out) == A20_WORK_NO_SPACE);
    uint32_t random = 1;
    for (unsigned i = 0; i < 100000; i++) {
        random = random * 1664525u + 1013904223u;
        unsigned len = random % sizeof(text);
        for (unsigned j = 0; j < len; j++)
            text[j] = (uint8_t)(random >> (j % 24));
        q.input = text;
        q.input_len = len;
        out = 132;
        (void)a20_work_word_map(0, &q, reference, &out);
        out = 132;
        (void)a20_work_word_merge(0, &q, reference, &out);
        (void)a20_word_dictionary_valid(text, len);
    }
    puts("test_clx_word_job: 1025 split/merge trees, generic/zero/CONCAT "
         "results, stale attempts, exact limits and 100K mutations PASS");
}

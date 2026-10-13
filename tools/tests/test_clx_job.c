#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "uapi/a20/cluster_job.h"

static const a20_work_operator_t ops[] = {
    {1, A20_WORK_REPLAY_SAFE, 140, 8, a20_work_dot, 0},
    {2, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 136, 8, a20_work_sum64,
     0}};
static const a20_work_agent_t agent = {.operators = ops,
                                       .operator_count = 2,
                                       .max_input = 140,
                                       .max_output = 8,
                                       .max_inflight = 1,
                                       .max_budget_ms = 30000};
static a20_job_target_t target(unsigned id) {
    a20_job_target_t t = {.boot_epoch = 100 + id,
                          .slot = 2,
                          .max_input = 140,
                          .max_output = 8,
                          .max_inflight = 1,
                          .flags = A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE};
    t.node_id[15] = id;
    return t;
}
static a20_job_task_t tasks[A20_JOB_DOT_TASKS];
static uint8_t inputs[8][132], vectors[1024], request[164], reply[40];
int main(void) {
    uint64_t expected = 0;
    for (unsigned i = 0; i < 128; i++) {
        a20_work_put32(vectors + i * 4, (uint32_t)(int32_t)((int)i - 64));
        a20_work_put32(vectors + (128 + i) * 4, i);
        expected += (uint64_t)((int64_t)((int)i - 64) * i);
    }
    a20_job_t job;
    a20_job_target_t a = target(1), b = target(2);
    int count = a20_job_dot_graph(tasks, 15, inputs, vectors, 128);
    assert(count == 15 &&
           a20_job_init(&job, 1, tasks, count, 0, 1000, 100, 3) == 0);
    int n = a20_job_begin(&job, 0, &a, 1, request, sizeof(request));
    assert(n == 164 && tasks[0].attempt == 1);
    /* One-inflight MCU admission and graph dependencies are hard bounds. */
    assert(a20_job_begin(&job, 1, &a, 1, request, sizeof(request)) < 0);
    assert(a20_job_begin(&job, 8, &b, 1, request, sizeof(request)) < 0);
    n = a20_work_dispatch(&agent, request, n, reply, sizeof(reply));
    uint8_t old_reply[40];
    memcpy(old_reply, reply, sizeof(reply));
    a20_job_node_failed(&job, a.node_id, a.boot_epoch);
    assert(tasks[0].state == A20_JOB_READY && job.retries == 1);
    n = a20_job_begin(&job, 0, &b, 2, request, sizeof(request));
    assert(n > 0 && tasks[0].attempt == 2);
    assert(a20_job_complete(&job, 0, a.node_id, a.boot_epoch, old_reply, 40,
                            3) == 1);
    assert(a20_job_complete(&job, 0, b.node_id, b.boot_epoch, old_reply, 40,
                            3) == 1);
    n = a20_work_dispatch(&agent, request, n, reply, sizeof(reply));
    assert(a20_job_complete(&job, 0, b.node_id, b.boot_epoch + 1, reply, n,
                            3) == 1);
    assert(a20_job_complete(&job, 0, b.node_id, b.boot_epoch, reply, n, 3) ==
           0);
    assert(a20_job_complete(&job, 0, b.node_id, b.boot_epoch, reply, n, 3) ==
           1);
    assert(job.completed == 1);
    for (unsigned now = 4; job.state == A20_JOB_RUNNING; now++) {
        assert(now < 1000);
        a20_job_tick(&job, now);
        for (unsigned i = 0; i < job.task_count; i++) {
            if (tasks[i].state != A20_JOB_READY)
                continue;
            a20_job_target_t *t = i % 2 ? &a : &b;
            n = a20_job_begin(&job, i, t, now, request, sizeof(request));
            assert(n > 0);
            n = a20_work_dispatch(&agent, request, n, reply, sizeof(reply));
            assert(a20_job_complete(&job, i, t->node_id, t->boot_epoch, reply,
                                    n, now) == 0);
        }
    }
    assert(job.state == A20_JOB_DONE && job.completed == 15 &&
           tasks[14].result == expected);
    /* Empty vectors and non-power-of-two trees preserve the root result. */
    for (unsigned elements = 0; elements <= 128; elements++) {
        count = a20_job_dot_graph(tasks, 15, inputs, vectors, elements);
        assert(count > 0 && count <= 15);
        assert(a20_job_init(&job, elements + 2, tasks, count, 0, 100, 10, 2) ==
               0);
        for (unsigned now = 1; job.state == A20_JOB_RUNNING; now++) {
            assert(now < 100);
            a20_job_tick(&job, now);
            for (unsigned i = 0; i < job.task_count; i++) {
                if (tasks[i].state != A20_JOB_READY)
                    continue;
                n = a20_job_begin(&job, i, &a, now, request, sizeof(request));
                assert(n > 0);
                n = a20_work_dispatch(&agent, request, n, reply, sizeof(reply));
                assert(a20_job_complete(&job, i, a.node_id, a.boot_epoch, reply,
                                        n, now) == 0);
            }
        }
        a20_work_request_t whole = {.input = request,
                                    .input_len = 4 + elements * 8};
        uint8_t whole_input[1028], expected_wire[8];
        a20_work_put32(whole_input, elements);
        memcpy(whole_input + 4, vectors, elements * 8);
        whole.input = whole_input;
        uint32_t out = 8;
        assert(a20_work_dot(0, &whole, expected_wire, &out) == 0);
        assert(tasks[count - 1].result == a20_work_u64(expected_wire));
    }
    /* Uncertain unsafe executions and process tasks fail without replay. */
    tasks[0] = (a20_job_task_t){.kind = A20_WORK_PROCESS,
                                .operator_id = 1,
                                .flags = A20_WORK_REPLAY_SAFE};
    assert(a20_job_init(&job, 200, tasks, 1, 0, 100, 10, 8) == 0);
    assert(a20_job_begin(&job, 0, &a, 1, request, sizeof(request)) > 0);
    a20_job_tick(&job, 11);
    assert(job.state == A20_JOB_FAILED && tasks[0].attempt == 1 &&
           !job.retries);
    tasks[0].kind = A20_WORK_OPERATOR;
    assert(a20_job_init(&job, 201, tasks, 1, 0, 100, 10, 2) == 0);
    a.flags = 0;
    assert(a20_job_begin(&job, 0, &a, 1, request, sizeof(request)) > 0);
    assert(a20_job_attempt_failed(&job, 0) < 0 && job.state == A20_JOB_FAILED);
    a = target(1);
    assert(a20_job_init(&job, 202, tasks, 1, 0, 100, 10, 2) == 0);
    for (unsigned i = 0; i < 2; i++) {
        assert(a20_job_begin(&job, 0, &a, 1 + i, request, sizeof(request)) > 0);
        a20_job_attempt_failed(&job, 0);
    }
    assert(job.state == A20_JOB_FAILED && tasks[0].attempt == 2);
    assert(a20_job_init(&job, 203, tasks, 1, 0, 100, 10, 2) == 0);
    a20_job_tick(&job, 100);
    assert(job.state == A20_JOB_DEADLINE);
    assert(a20_job_init(&job, 204, tasks, 1, 0, 100, 10, 2) == 0);
    a20_job_cancel(&job);
    assert(job.state == A20_JOB_CANCELED);
    tasks[0].children[0] = 0;
    tasks[0].child_count = 1;
    tasks[0].operator_id = 2;
    tasks[0].flags |= A20_WORK_ASSOCIATIVE;
    assert(a20_job_init(&job, 205, tasks, 1, 0, 100, 10, 2) < 0);
    puts("test_clx_job: 129 bounded trees, mixed admission, reassign, "
         "stale/duplicate results, unsafe failure, deadlines and cancellation "
         "passed");
}

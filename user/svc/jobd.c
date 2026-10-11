/* Bounded single-job Native coordinator. Each request owns its complete graph
 * until the reply; no background worker can outlive the caller's storage. */
#include "liba20rt/a20_cluster_job.h"
#include "liba20rt/crt0_a20.h"
#include "../../kernel/include/uapi/a20/cluster_word_job.h"

static a20_job_task_t tasks[A20_JOB_WORD_TASKS];
static uint8_t inputs[A20_JOB_DOT_LEAVES][132];
static uint8_t outputs[A20_JOB_WORD_TASKS][A20_WORD_RESULT];
static uint8_t word_inputs[A20_JOB_WORD_LEAVES][A20_JOB_WORD_CHUNK];
static uint64_t sequence;
static int dispatch(void *ctx, const uint8_t *request, uint32_t len,
                    uint8_t *reply, uint32_t capacity) {
    (void)ctx;
    if (capacity < 40)
        return -1;
    a20_memset(reply, 0, 40);
    reply[0] = 'C';
    reply[1] = 'J';
    reply[2] = 1;
    reply[3] = A20_JOB_DOT;
    a20_work_put32(reply + 12, A20_WORK_INVALID);
    if (len < A20_JOB_WIRE_HEADER || request[0] != 'C' || request[1] != 'J' ||
        request[2] != 1 ||
        (request[3] != A20_JOB_DOT && request[3] != A20_JOB_WORDCOUNT) ||
        !a20_work_u64(request + 4) || a20_work_u32(request + 20) ||
        a20_work_u64(request + 24))
        return 40;
    int words = request[3] == A20_JOB_WORDCOUNT;
    reply[3] = request[3];
    unsigned error_len = words ? 32 : 40;
    if (words && capacity < 32 + A20_WORD_RESULT)
        return -1;
    uint64_t caller_id = a20_work_u64(request + 4);
    a20_work_put64(reply + 4, caller_id);
    uint32_t count = a20_work_u32(request + 16);
    uint32_t budget = a20_work_u32(request + 12);
    if (count > (words ? A20_JOB_MAX_TEXT : A20_JOB_MAX_ELEMENTS) ||
        len != A20_JOB_WIRE_HEADER + count * (words ? 1 : 8) || !budget ||
        budget > 30000)
        return error_len;
    int n = words ? a20_job_word_graph(tasks, A20_JOB_WORD_TASKS, word_inputs,
                                       outputs, request + A20_JOB_WIRE_HEADER,
                                       count)
                  : a20_job_dot_graph(tasks, A20_JOB_DOT_TASKS, inputs,
                                      request + A20_JOB_WIRE_HEADER, count);
    a20_job_t job;
    /* Use owner-generated IDs for CW execution. A repeated client CJ request
     * is a new job, never an alias of an in-progress caller-chosen shard. */
    if (sequence == UINT64_MAX || n < 0 ||
        a20_job_init(&job, ++sequence, tasks, n, a20_agent_clock_ms(0), budget,
                     budget < 500 ? budget : 500, 8) < 0)
        return error_len;
    int ok = a20_cluster_job_run(&job, "work");
    uint32_t status = ok == 0                         ? A20_WORK_OK
                      : job.state == A20_JOB_CANCELED ? A20_WORK_CANCELED
                      : job.state == A20_JOB_DEADLINE ? A20_WORK_DEADLINE
                                                      : A20_WORK_FAILED;
    a20_work_put32(reply + 12, status);
    a20_work_put32(reply + 16, job.completed);
    a20_work_put32(reply + 20, job.retries);
    a20_work_put32(reply + 24, job.task_count);
    if (words) {
        unsigned n = status ? 0 : tasks[job.task_count - 1].output_len;
        a20_work_put32(reply + 28, n);
        if (n)
            a20_memcpy(reply + 32, tasks[job.task_count - 1].output, n);
        return 32 + n;
    }
    if (!status)
        a20_work_put64(reply + 32, tasks[job.task_count - 1].result);
    return 40;
}
int main(int argc, char **argv, char **envp) {
    (void)argc;
    (void)argv;
    (void)envp;
    a20_cluster_snapshot_args_t self = {0};
    if (a20_cluster_snapshot(&self) < 0)
        return 1;
    /* A per-coordinator boot prefix avoids collisions after daemon restart;
     * the counter never wraps. CL transaction dedup remains separate. */
    sequence = (a20_agent_clock_ms(0) << 24) ^ self.boot_epoch;
    sequence &= UINT64_MAX >> 1;
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) < 0 ||
        a20_cluster_export(pair.endpoints[0], "cluster.jobs",
                           A20_EXPORT_REPLACE | A20_EXPORT_LOCAL_ONLY, 0) < 0)
        return 2;
    a20_hdl_close(pair.endpoints[1]);
    a20_cluster_agent_server_t server;
    a20_work_agent_t unused = {0};
    a20_cluster_agent_init(&server, pair.endpoints[0], &unused);
    server.dispatch = dispatch;
    for (;;) {
        if (a20_signal_check() & (1ULL << 15))
            return 0;
        if (a20_cluster_agent_pump(&server) < 0)
            return 3;
        a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
}

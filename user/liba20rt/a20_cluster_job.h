#ifndef A20_NATIVE_CLUSTER_JOB_H
#define A20_NATIVE_CLUSTER_JOB_H
#include "a20_cluster_client.h"
#include "../../kernel/include/uapi/a20/cluster_job.h"

/* Native transport driver for the same graph state machine used by firmware
 * harnesses. Fresh endpoints isolate each attempt and are closed on every
 * termination path. The caller owns task storage; this bounded driver accepts
 * at most the convenience DOT graph's 15 nodes. */
static inline int a20_cluster_job_run(a20_job_t *job, const char *service) {
    if (!job || job->task_count > A20_JOB_DOT_TASKS || !service)
        return -1;
    a20_handle_t endpoints[A20_JOB_DOT_TASKS];
    for (uint32_t i = 0; i < A20_JOB_DOT_TASKS; i++)
        endpoints[i] = A20_HANDLE_NULL;
    a20_cluster_snapshot_args_t self = {0};
    if (a20_cluster_snapshot(&self) < 0)
        return -1;
    while (job->state == A20_JOB_RUNNING) {
        uint64_t now = a20_agent_clock_ms(0);
        if (a20_signal_check() & (1ULL << 15))
            a20_job_cancel(job);
        a20_job_tick(job, now);
        for (uint32_t i = 0; i < job->task_count; i++) {
            if (endpoints[i] == A20_HANDLE_NULL)
                continue;
            if (job->state != A20_JOB_RUNNING ||
                job->tasks[i].state != A20_JOB_RUNNING) {
                a20_hdl_close(endpoints[i]);
                endpoints[i] = A20_HANDLE_NULL;
                continue;
            }
            uint8_t reply[40];
            uint32_t len = sizeof(reply), handles = 0;
            a20_status_t r = a20_channel_recv_flags(
                endpoints[i], reply, &len, 0, &handles, A20_MSG_NONBLOCK);
            if (r == -A20_ERR_WOULD_BLOCK)
                continue;
            a20_hdl_close(endpoints[i]);
            endpoints[i] = A20_HANDLE_NULL;
            if (r < 0) {
                a20_job_attempt_failed(job, i);
            } else {
                a20_job_task_t *t = &job->tasks[i];
                int committed = a20_job_complete(job, i, t->target.node_id,
                                                 t->target.boot_epoch, reply,
                                                 len, a20_agent_clock_ms(0));
                if (committed == 1 && job->state == A20_JOB_RUNNING &&
                    t->state == A20_JOB_RUNNING)
                    a20_job_attempt_failed(job, i);
            }
        }
        a20_job_tick(job, a20_agent_clock_ms(0));
        for (uint32_t i = 0;
             i < job->task_count && job->state == A20_JOB_RUNNING; i++) {
            a20_job_task_t *t = &job->tasks[i];
            if (t->state != A20_JOB_READY)
                continue;
            now = a20_agent_clock_ms(0);
            if (now >= job->deadline)
                break;
            uint64_t remaining = job->deadline - now;
            uint32_t lookup_ms = remaining < 100 ? (uint32_t)remaining : 100;
            a20_cluster_selection_t selected;
            if (a20_cluster_resolve_any(service, t->kind, t->operator_id,
                                        a20_job_input_len(t), lookup_ms,
                                        &selected) < 0)
                continue;
            a20_job_target_t target = {.boot_epoch = selected.boot_epoch,
                                       .slot = selected.slot,
                                       .max_input = selected.max_input,
                                       .max_output = selected.max_output,
                                       .max_inflight = selected.max_inflight,
                                       .flags = selected.flags};
            a20_memcpy(target.node_id, selected.node.bytes, 16);
            uint8_t wire[A20_WORK_HEADER + A20_JOB_MAX_INPUT];
            now = a20_agent_clock_ms(0);
            int n = a20_job_begin(job, i, &target, now, wire, sizeof(wire));
            if (n < 0)
                continue;
            a20_node_id_t node = selected.node;
            if (!a20_memcmp(node.bytes, self.node_id, 16))
                a20_node_id_local(&node);
            a20_status_t r = a20_cluster_connect_bound(
                &node, selected.slot, selected.boot_epoch, &endpoints[i]);
            if (r >= 0)
                r = a20_channel_send_flags(endpoints[i], wire, n, 0, 0,
                                           A20_MSG_NONBLOCK);
            if (r < 0) {
                if (endpoints[i] != A20_HANDLE_NULL)
                    a20_hdl_close(endpoints[i]);
                endpoints[i] = A20_HANDLE_NULL;
                a20_job_attempt_failed(job, i);
            }
        }
        if (job->state == A20_JOB_RUNNING)
            a20_thread_sleep((a20_time_t){.nsecs = 1000000});
    }
    for (uint32_t i = 0; i < job->task_count; i++)
        if (endpoints[i] != A20_HANDLE_NULL)
            a20_hdl_close(endpoints[i]);
    return job->state == A20_JOB_DONE ? 0 : -1;
}
#endif

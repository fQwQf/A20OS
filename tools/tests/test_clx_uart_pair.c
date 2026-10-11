/* Execute BOTH shipped UART adapters on a deterministic byte-stream wire.
 * Only the board byte I/O, lock and monotonic clock are replaced. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "uapi/a20/cluster_job.h"

#define CONFIG_MCU 1
#define CONFIG_CLUSTER_PROFILE 2
#define _CORE_LOCK_H
#define _TIMER_H
#define TICKS_PER_SEC 1000u
#define MS_TO_TICKS(ms) ((uint64_t)(ms))
typedef struct {
    int held;
} spinlock_t;
#define SPINLOCK_INIT {0}
static uint64_t spin_lock_irqsave(spinlock_t *lock) {
    assert(!lock->held);
    lock->held = 1;
    return 0;
}
static void spin_unlock_irqrestore(spinlock_t *lock, uint64_t flags) {
    (void)flags;
    assert(lock->held);
    lock->held = 0;
}
static uint64_t ticks;
uint64_t timer_get_ticks(void) {
    return ticks;
}
int klog_level = 4;
void klog_write(const char *format, ...) {
    (void)format;
}

#define printf a20_test_kernel_printf
#include "../../kernel/cluster/uart.c"
#include "../../kernel/mcu/leaf.c"
#undef printf

typedef struct {
    uint8_t data[4096];
    unsigned head, tail;
} wire_queue;
static wire_queue to_head, to_leaf;
static int unplugged;
static int advance_head_rx_clock = 1, advance_leaf_rx_clock;
static uint8_t received[256];
static unsigned received_len, replies;
static int link_up;
static unsigned link_down_events;

static void wire_put(wire_queue *q, uint8_t byte) {
    assert(!g_uart_lock.held);
    if (unplugged)
        return;
    assert(q->tail - q->head < sizeof(q->data));
    q->data[q->tail++ % sizeof(q->data)] = byte;
}
static int wire_get(wire_queue *q) {
    assert(!g_uart_lock.held);
    return q->head == q->tail ? -1 : q->data[q->head++ % sizeof(q->data)];
}
void uart_putc(char c) {
    wire_put(&to_head, (uint8_t)c);
}
int uart_try_getc(void) {
    return wire_get(&to_leaf);
}
static int leaf_raw_getc(void *ctx) {
    assert(ctx == &to_leaf);
    int byte = wire_get(ctx);
    if (byte >= 0 && advance_leaf_rx_clock) {
        ticks++;
        advance_leaf_rx_clock = 0;
    }
    return byte;
}
static int leaf_raw_putc(void *ctx, uint8_t byte) {
    assert(ctx == &to_leaf);
    wire_put(&to_head, byte);
    return 0;
}
static int head_getc(void *ctx) {
    (void)ctx;
    int byte = wire_get(&to_head);
    if (byte >= 0 && advance_head_rx_clock) {
        ticks++;
        advance_head_rx_clock = 0;
    }
    return byte;
}
static int head_putc(void *ctx, uint8_t b) {
    (void)ctx;
    wire_put(&to_leaf, b);
    return 0;
}
static void on_rx(void *ctx, const uint8_t *nh, uint32_t nh_len,
                  const uint8_t *frame, uint32_t len) {
    (void)ctx;
    assert(nh_len == 2 && a20_clx_get_le16(nh) == 7);
    assert(len <= sizeof(received));
    memcpy(received, frame, len);
    received_len = len;
    replies++;
}
static void on_link(void *ctx, const uint8_t *nh, uint32_t len, int up) {
    (void)ctx;
    (void)nh;
    assert(len == 2 && !g_uart_lock.held);
    link_up = up;
    if (!up)
        link_down_events++;
}
static int inject_byte(void *ctx, uint8_t byte) {
    wire_put((wire_queue *)ctx, byte);
    return 0;
}
static void inject_frame(wire_queue *queue, const uint8_t *frame,
                         unsigned len) {
    wire_put(queue, A20_CLX_SLIP_END);
    for (unsigned i = 0; i < len; i++)
        assert(a20_clx_slip_emit(frame[i], inject_byte, queue) == 0);
    wire_put(queue, A20_CLX_SLIP_END);
}

static void pump(unsigned milliseconds) {
    for (unsigned i = 0; i < milliseconds; i += 5) {
        ticks += 5;
        a20_clx_uart_poll_all();
        a20_mcu_leaf_poll();
        a20_clx_uart_poll_all();
        a20_clx_uart_link_status_t status;
        if (a20_clx_uart_get_status(0, &status) == 0 &&
            status.state == A20_CLX_UART_L_UP)
            assert(status.last_hello_age_ms <= ticks);
    }
}

int main(void) {
    a20_mcu_leaf_init();
    assert(a20_mcu_leaf_set_io(NULL, leaf_raw_getc, &to_leaf) < 0);
    assert(a20_mcu_leaf_set_io(leaf_raw_putc, leaf_raw_getc, &to_leaf) == 0);
    uint8_t leaf_id[16] = {0};
    leaf_id[15] = 2;
    assert(a20_mcu_leaf_configure(leaf_id, 200) == 0);
    a20_clx_uart_link_cfg_t cfg = {0};
    cfg.self_node_id[15] = 1;
    cfg.assigned_addr = 7;
    cfg.session = 100;
    cfg.drv.getc = head_getc;
    cfg.drv.putc = head_putc;
    int id = a20_clx_uart_link_register(&cfg);
    assert(id >= 0);
    a20_clx_uart_set_rx_handler(on_rx, NULL);
    a20_clx_uart_set_link_event_handler(on_link, NULL);
    pump(3000);
    assert(link_up);
    assert(a20_mcu_leaf_set_io(leaf_raw_putc, leaf_raw_getc, &to_leaf) < 0);
    uint32_t old_head_cookie = g_uart_hello_counter;
    uint32_t old_leaf_cookie = leaf_hello_counter;
    a20_mcu_leaf_status_t leaf;
    a20_mcu_leaf_get_status(&leaf);
    assert(leaf.state == A20_MCU_LEAF_UP && leaf.short_addr == 7);

    uint8_t request[12], frame[256], nh[2] = {7, 0};
    a20_clx_put_le32(request, 1);
    a20_clx_put_le32(request + 4, (uint32_t)-7);
    a20_clx_put_le32(request + 8, 6);
    int n = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_CALL, 0, 0,
                                123, a20_clx_fnv1a32(cfg.self_node_id),
                                a20_clx_fnv1a32(leaf_id), 1, request,
                                sizeof(request));
    assert(n > 0);
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    assert(replies == 1 && received_len == 46);
    assert(received[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_CALL_REPLY);
    assert(a20_clx_get_le32(received + A20_CLX_OFF_TXID) == 123);
    assert(a20_clx_get_le32(received + A20_CLX_OFF_PAYLOAD) == 0);
    assert((int64_t)a20_clx_get_le64(received + A20_CLX_OFF_PAYLOAD + 4) ==
           -42);
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    a20_mcu_leaf_get_status(&leaf);
    assert(replies == 2 && leaf.counters.dedup_drops == 1);

    uint64_t local, remote;
    uint32_t caps;
    assert(a20_clx_uart_session(nh, 2, leaf_id, &local, &remote, &caps) == 0);
    assert(caps & A20_CLX_CAP_RELIABLE);
    uint8_t reliable_payload[28];
    a20_clx_session_encode(reliable_payload, local, remote);
    memcpy(reliable_payload + 16, request, sizeof(request));
    n = a20_clx_build_frame(
        frame, sizeof(frame), A20_CLX_TYPE_CALL, A20_CLX_FLAG_RELIABLE, 0, 124,
        a20_clx_fnv1a32(cfg.self_node_id), a20_clx_fnv1a32(leaf_id), 1,
        reliable_payload, sizeof(reliable_payload));
    leaf_set_reliable_seq(frame, n, 124);
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    assert(received[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_CALL_REPLY);
    assert(a20_clx_session_matches(received + 32, received_len - 34, remote,
                                   local));
    assert(a20_clx_get_le32(received + 48) == 0);
    assert((int64_t)a20_clx_get_le64(received + 56) == -42);
    /* Lose the reply at the caller and repeat the exact request. */
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    a20_mcu_leaf_get_status(&leaf);
    assert(leaf.counters.dedup_drops == 2);
    assert((int64_t)a20_clx_get_le64(received + 56) == -42);
    leaf_set_reliable_seq(frame, n, 125);
    a20_clx_put_le32(frame + A20_CLX_OFF_TXID, 125);
    leaf_set_reliable_seq(frame, n, 125);
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    assert(a20_clx_get_le32(received + 48) ==
           13); /* Cache full, no execution. */
    uint8_t receipt[64];
    int receipt_len = a20_clx_build_frame(
        receipt, sizeof(receipt), A20_CLX_TYPE_CALL, A20_CLX_FLAG_RELIABLE, 0,
        124, a20_clx_fnv1a32(cfg.self_node_id), a20_clx_fnv1a32(leaf_id), 0,
        reliable_payload, 16);
    leaf_set_reliable_seq(receipt, receipt_len, 124);
    assert(a20_clx_uart_send_frame(nh, 2, receipt, receipt_len) == 0);
    pump(50);
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    assert(a20_clx_get_le32(received + 48) == 0);
    a20_clx_put_le32(frame + A20_CLX_OFF_TXID, 124);
    leaf_set_reliable_seq(frame, n, 124);
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    assert(a20_clx_get_le32(received + 48) ==
           20); /* Retired transaction, no replay. */
    a20_clx_put_le64(frame + 40, remote ^ 1);
    leaf_set_reliable_seq(frame, n, 124);
    unsigned before = replies;
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    assert(replies == before); /* Old receiver session is never executed. */
    /* Restore the legacy frame for reconnect regression. */
    n = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_CALL, 0, 0, 123,
                            a20_clx_fnv1a32(cfg.self_node_id),
                            a20_clx_fnv1a32(leaf_id), 1, request,
                            sizeof(request));
    unplugged = 1;
    pump(55000);
    assert(!link_up);
    a20_mcu_leaf_get_status(&leaf);
    assert(leaf.state == A20_MCU_LEAF_DISCONNECTED);
    unplugged = 0;
    pump(10000);
    assert(link_up);
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    assert(replies == before + 1);
    /* The same client request and same operator execute through the real
     * UART leaf and a hosted agent with a larger resource budget. */
    const a20_work_operator_t host_ops[] = {
        {A20_WORK_DOT, A20_WORK_REPLAY_SAFE, 1024, 8, a20_work_dot, NULL},
        {A20_WORK_SUM64, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 1024, 8,
         a20_work_sum64, NULL},
    };
    const a20_work_agent_t host = {.operators = host_ops,
                                   .operator_count = 2,
                                   .max_input = 1024,
                                   .max_output = 1024,
                                   .max_inflight = 1,
                                   .max_budget_ms = 30000};
    uint8_t work_buf[160], host_reply[160];
    for (unsigned kind = A20_WORK_QUERY; kind <= A20_WORK_PROCESS; kind++) {
        a20_work_request_t work = {
            .kind = kind,
            .job = 77,
            .shard = 5,
            .operator_id = kind == A20_WORK_QUERY ? 0 : A20_WORK_DOT,
            .budget_ms = 1000,
            .attempt = 3,
            .input = request,
            .input_len = kind == A20_WORK_QUERY ? 0 : sizeof(request)};
        int work_len = a20_work_encode(work_buf, sizeof(work_buf), &work);
        n = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_CALL, 0, 0,
                                200 + kind, a20_clx_fnv1a32(cfg.self_node_id),
                                a20_clx_fnv1a32(leaf_id),
                                A20_MCU_LEAF_WORK_SLOT, work_buf, work_len);
        assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
        pump(50);
        a20_work_result_t result;
        assert(a20_work_result_decode(received + 32, received_len - 34, &work,
                                      &result) == 0);
        if (kind == A20_WORK_QUERY) {
            assert(result.status == 0 && result.output_len == 56);
            assert(a20_work_u32(result.output) == A20_WORK_CAP_OPERATOR);
            assert(a20_work_u32(result.output + 4) == 140);
        } else {
            int host_len = a20_work_dispatch(&host, work_buf, work_len,
                                             host_reply, sizeof(host_reply));
            assert(host_len == (int)received_len - 34);
            assert(memcmp(host_reply, received + 32, host_len) == 0);
            assert(result.status == (kind == A20_WORK_OPERATOR
                                         ? A20_WORK_OK
                                         : A20_WORK_UNSUPPORTED));
            if (kind == A20_WORK_OPERATOR)
                assert((int64_t)a20_work_u64(result.output) == -42);
        }
    }
    uint8_t directory_request[A20_DIRECTORY_REQUEST];
    a20_directory_request(directory_request, 1, 0);
    n = a20_clx_build_frame(frame, sizeof(frame), A20_CLX_TYPE_CALL, 0, 0, 205,
                            a20_clx_fnv1a32(cfg.self_node_id),
                            a20_clx_fnv1a32(leaf_id), A20_DIRECTORY_SLOT,
                            directory_request, sizeof(directory_request));
    assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
    pump(50);
    a20_directory_record_t directory;
    assert(a20_directory_decode(received + 32, received_len - 34, leaf_id,
                                &directory) == 0);
    assert(directory.status == A20_DIRECTORY_FOUND &&
           directory.slot == A20_MCU_LEAF_WORK_SLOT);
    assert(directory.name_len == 4 && !memcmp(directory.name, "work", 4));
    assert(directory.boot_epoch == remote);
    /* One real UART leaf and one hosted agent execute a single binary tree.
     * Lose the leaf root's reply AFTER execution, then reassign that shard.
     * The retired attempt must never add its result a second time. */
    a20_job_task_t graph[A20_JOB_DOT_TASKS];
    uint8_t shards[8][132], vectors[512], request_wire[164], result_wire[40];
    uint64_t expected = 0;
    for (unsigned i = 0; i < 64; i++) {
        a20_work_put32(vectors + i * 4, (uint32_t)(int32_t)((int)i - 32));
        a20_work_put32(vectors + (64 + i) * 4, i);
        expected += (uint64_t)((int64_t)((int)i - 32) * i);
    }
    int task_count = a20_job_dot_graph(graph, 15, shards, vectors, 64);
    assert(task_count == 7);
    a20_job_t job;
    assert(a20_job_init(&job, 500, graph, task_count, ticks, 5000, 200, 3) ==
           0);
    a20_job_target_t mcu_target = {.boot_epoch = remote,
                                   .slot = 2,
                                   .max_input = 140,
                                   .max_output = 8,
                                   .max_inflight = 1,
                                   .flags = A20_WORK_REPLAY_SAFE |
                                            A20_WORK_ASSOCIATIVE};
    memcpy(mcu_target.node_id, leaf_id, 16);
    a20_job_target_t host_target = mcu_target;
    memcpy(host_target.node_id, cfg.self_node_id, 16);
    host_target.boot_epoch = local;
    host_target.max_input = 1024;
    unsigned mcu_tasks = 0, host_tasks = 0;
    uint8_t delayed_root[40];
    while (job.state == A20_JOB_RUNNING) {
        a20_job_tick(&job, ticks);
        for (unsigned i = 0; i < job.task_count; i++) {
            if (graph[i].state != A20_JOB_READY)
                continue;
            int is_mcu = !(i % 2) && !graph[i].attempt;
            a20_job_target_t *target = is_mcu ? &mcu_target : &host_target;
            int request_len = a20_job_begin(&job, i, target, ticks,
                                            request_wire, sizeof(request_wire));
            assert(request_len > 0);
            int result_len;
            if (is_mcu) {
                uint8_t payload[180];
                uint32_t txid = 500 + i * 8 + graph[i].attempt;
                a20_clx_session_encode(payload, local, remote);
                memcpy(payload + 16, request_wire, request_len);
                n = a20_clx_build_frame(
                    frame, sizeof(frame), A20_CLX_TYPE_CALL,
                    A20_CLX_FLAG_RELIABLE, 0, txid,
                    a20_clx_fnv1a32(cfg.self_node_id), a20_clx_fnv1a32(leaf_id),
                    A20_MCU_LEAF_WORK_SLOT, payload, 16 + request_len);
                leaf_set_reliable_seq(frame, n, txid);
                assert(a20_clx_uart_send_frame(nh, 2, frame, n) == 0);
                pump(50);
                assert(received[A20_CLX_OFF_TYPE] == A20_CLX_TYPE_CALL_REPLY &&
                       a20_clx_get_le32(received + 48) == 0);
                result_len = received_len - 54;
                assert(result_len == 40);
                memcpy(result_wire, received + 52, result_len);
                mcu_tasks++;
                if (i == job.task_count - 1) {
                    memcpy(delayed_root, result_wire, 40);
                    a20_job_node_failed(&job, leaf_id, remote);
                    assert(graph[i].state == A20_JOB_READY);
                    continue;
                }
                int receipt_len = a20_clx_build_frame(
                    receipt, sizeof(receipt), A20_CLX_TYPE_CALL,
                    A20_CLX_FLAG_RELIABLE, 0, txid,
                    a20_clx_fnv1a32(cfg.self_node_id), a20_clx_fnv1a32(leaf_id),
                    0, payload, 16);
                leaf_set_reliable_seq(receipt, receipt_len, txid);
                assert(a20_clx_uart_send_frame(nh, 2, receipt, receipt_len) ==
                       0);
                pump(5);
            } else {
                result_len =
                    a20_work_dispatch(&host, request_wire, request_len,
                                      result_wire, sizeof(result_wire));
                host_tasks++;
            }
            assert(a20_job_complete(&job, i, target->node_id,
                                    target->boot_epoch, result_wire, result_len,
                                    ticks) == 0);
        }
    }
    assert(job.state == A20_JOB_DONE && graph[6].result == expected &&
           job.completed == 7 && job.retries == 1 && mcu_tasks == 4 &&
           host_tasks == 4);
    assert(a20_job_complete(&job, 6, leaf_id, remote, delayed_root, 40,
                            ticks) == 1);
    puts("test_clx_uart_pair: mixed UART/host job, reliable shards, root "
         "reassign and duplicate reduction passed");
    /* An old HELLO can request a fresh challenge, but cannot erase the
     * protected result cache or roll back a peer boot epoch. */
    uint8_t hello[32], stale[80];
    uint16_t protected_len = leaf_inflight_len;
    uint32_t protected_txid = leaf_inflight_txid;
    unsigned downs = link_down_events;
    a20_clx_build_hello_payload(cfg.self_node_id, 0, 1, 2, A20_CLX_CAP_RELIABLE,
                                7, 99, hello);
    n = a20_clx_build_frame(stale, sizeof(stale), A20_CLX_TYPE_HELLO, 0, 0, 900,
                            a20_clx_fnv1a32(cfg.self_node_id),
                            a20_clx_fnv1a32(leaf_id), 0, hello, 32);
    inject_frame(&to_leaf, stale, n);
    pump(100);
    assert(leaf_peer_nonce == 100 && leaf_inflight_len == protected_len &&
           leaf_inflight_txid == protected_txid && link_down_events == downs);
    a20_mcu_leaf_init();
    assert(a20_mcu_leaf_set_io(NULL, leaf_raw_getc, &to_leaf) < 0);
    assert(a20_mcu_leaf_set_io(leaf_raw_putc, leaf_raw_getc, &to_leaf) == 0);
    assert(a20_mcu_leaf_configure(leaf_id, 300) == 0);
    a20_clx_build_hello_payload(leaf_id, 0, 1, 1, A20_CLX_CAP_RELIABLE, 7, 300,
                                hello);
    assert(leaf_send(A20_CLX_TYPE_HELLO, 901, a20_clx_fnv1a32(leaf_id),
                     a20_clx_fnv1a32(cfg.self_node_id), 0, hello, 32) == 0);
    pump(200);
    assert(link_up && leaf_state == A20_MCU_LEAF_UP &&
           a20_clx_uart_session(nh, 2, leaf_id, &local, &remote, &caps) == 0 &&
           local == 100 && remote == 300 && link_down_events == downs + 1);
    /* Replay an ACK from before the leaf reboot, then an old leaf HELLO. */
    a20_clx_put_le64(hello + A20_CLX_HELLO_OFF_NONCE, 200);
    n = a20_clx_build_frame(stale, sizeof(stale), A20_CLX_TYPE_HELLO_ACK, 0, 0,
                            old_head_cookie, a20_clx_fnv1a32(leaf_id),
                            a20_clx_fnv1a32(cfg.self_node_id), 0, hello, 32);
    inject_frame(&to_head, stale, n);
    n = a20_clx_build_frame(stale, sizeof(stale), A20_CLX_TYPE_HELLO, 0, 0, 902,
                            a20_clx_fnv1a32(leaf_id),
                            a20_clx_fnv1a32(cfg.self_node_id), 0, hello, 32);
    inject_frame(&to_head, stale, n);
    pump(100);
    assert(a20_clx_uart_session(nh, 2, leaf_id, &local, &remote, &caps) == 0 &&
           remote == 300 && link_down_events == downs + 1);
    assert(a20_clx_uart_link_unregister(id) == 0 && !link_up);
    cfg.session = 400;
    id = a20_clx_uart_link_register(&cfg);
    assert(id >= 0);
    pump(3000);
    assert(link_up && leaf_peer_nonce == 400);
    a20_clx_build_hello_payload(cfg.self_node_id, 0, 1, 2, A20_CLX_CAP_RELIABLE,
                                7, 100, hello);
    n = a20_clx_build_frame(stale, sizeof(stale), A20_CLX_TYPE_HELLO_ACK, 0, 0,
                            old_leaf_cookie, a20_clx_fnv1a32(cfg.self_node_id),
                            a20_clx_fnv1a32(leaf_id), 0, hello, 32);
    inject_frame(&to_leaf, stale, n);
    n = a20_clx_build_frame(stale, sizeof(stale), A20_CLX_TYPE_HELLO, 0, 0, 903,
                            a20_clx_fnv1a32(cfg.self_node_id),
                            a20_clx_fnv1a32(leaf_id), 0, hello, 32);
    inject_frame(&to_leaf, stale, n);
    pump(100);
    assert(leaf_peer_nonce == 400 && link_up &&
           a20_clx_uart_session(nh, 2, leaf_id, &local, &remote, &caps) == 0 &&
           local == 400 && remote == 300);
    /* Reproduce a real clock tick between poll entry and PING handling.
     * A stale subtraction used to underflow and disconnect immediately. */
    uint8_t heartbeat[8] = {0};
    n = a20_clx_build_frame(stale, sizeof(stale), A20_CLX_TYPE_PING, 0, 0, 0,
                            a20_clx_fnv1a32(cfg.self_node_id),
                            a20_clx_fnv1a32(leaf_id), 0, heartbeat, 8);
    inject_frame(&to_leaf, stale, n);
    advance_leaf_rx_clock = 1;
    a20_mcu_leaf_poll();
    assert(!advance_leaf_rx_clock && leaf_state == A20_MCU_LEAF_UP &&
           leaf_last_ping_ticks == ticks);
    assert(a20_clx_uart_link_unregister(id) == 0 && !link_up);
    puts("test_clx_uart_pair: bidirectional challenge, both reboots, stale "
         "HELLO/ACK, cache preservation and DOWN events passed");

    puts("test_clx_uart_pair: handshake, shared dot, reliable replay, cache "
         "pressure, stale session, reconnect, mixed-job reassign and "
         "unregister passed");
    return 0;
}

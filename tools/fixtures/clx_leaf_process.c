/* Run the shipped MCU leaf on a host byte stream. Only board I/O and the
 * monotonic clock are replaced; this is not physical MCU evidence. */
#include <assert.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>
#define CONFIG_MCU 1
#define CONFIG_CLUSTER_PROFILE 1
#define _UART_H
void uart_putc(char c);
int uart_try_getc(void);
#define _TIMER_H
#define TICKS_PER_SEC 1000u
#define MS_TO_TICKS(ms) ((uint64_t)(ms))
uint64_t timer_get_ticks(void) {
    struct timespec t;
    assert(clock_gettime(CLOCK_MONOTONIC, &t) == 0);
    return (uint64_t)t.tv_sec * 1000 + t.tv_nsec / 1000000;
}
#define printf a20_test_kernel_printf
#include "../../kernel/mcu/leaf.c"
#undef printf
void uart_putc(char c) {
    (void)c;
    assert(0);
}
int uart_try_getc(void) {
    assert(0);
    return -1;
}
static int raw_send(void *ctx, uint8_t byte) {
    (void)ctx;
    return write(STDOUT_FILENO, &byte, 1) == 1 ? 0 : -1;
}
static int raw_receive(void *ctx) {
    (void)ctx;
    uint8_t byte;
    return read(STDIN_FILENO, &byte, 1) == 1 ? byte : -1;
}
static unsigned executions;
static unsigned sink_count, sink_duplicates, sink_bad;
static uint8_t sink_seen[128];
static uint32_t counted_sink(void *ctx, const uint8_t *input, uint32_t len) {
    (void)ctx;
    unsigned index = len >= 8 ? a20_work_u32(input) : 128;
    if (index >= 128 || a20_work_u32(input + 4) != len) {
        sink_bad++;
        return 12; /* Native INVALID_ARGUMENT, carried by the SEND receipt. */
    }
    for (unsigned i = 8; i < len; i++)
        if (input[i] != (uint8_t)(index ^ i)) {
            sink_bad++;
            return 12;
        }
    if (sink_seen[index])
        sink_duplicates++;
    sink_seen[index] = 1;
    fprintf(stderr, "SEND-EXEC:%u index=%u bytes=%u duplicates=%u bad=%u\n",
            ++sink_count, index, len, sink_duplicates, sink_bad);
    return 0;
}
static uint32_t counted_dot(void *ctx, const a20_work_request_t *request,
                            uint8_t *output, uint32_t *output_len) {
    /* Fixture application query, executed through the shipped work dispatch.
     * It does not create a synthetic wire reply or change production DOT. */
    if (request->job == 0x53454e44u) {
        if (*output_len < 8)
            return A20_WORK_NO_SPACE;
        a20_work_put64(output, sink_count | ((uint64_t)sink_duplicates << 32) |
                                   ((uint64_t)sink_bad << 48));
        *output_len = 8;
        return 0;
    }
    fprintf(stderr, "DOT-EXEC:%u job=%llu shard=%u attempt=%u\n", ++executions,
            (unsigned long long)request->job, request->shard, request->attempt);
    return a20_work_dot(ctx, request, output, output_len);
}
static uint32_t counted_map(void *ctx, const a20_work_request_t *request,
                            uint8_t *output, uint32_t *output_len) {
    fprintf(stderr, "MAP-EXEC: job=%llu shard=%u attempt=%u\n",
            (unsigned long long)request->job, request->shard, request->attempt);
    return a20_work_word_map(ctx, request, output, output_len);
}
int main(int argc, char **argv) {
    int oneway = argc > 1 && !strcmp(argv[argc - 1], "--send");
    if (oneway)
        argc--;
    assert(fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == 0);
    a20_mcu_leaf_init();
    a20_work_operator_t ops[sizeof(leaf_operators) / sizeof(leaf_operators[0])];
    memcpy(ops, leaf_operators, sizeof(ops));
    ops[0].run = counted_dot;
    ops[2].run = counted_map;
    a20_work_agent_t agent = leaf_default_agent;
    agent.operators = ops;
    uint8_t id[16] = {0};
    id[15] = 2;
    if (argc == 3) {
        assert(!strcmp(argv[1], "--node") &&
               (!strcmp(argv[2], "7") || !strcmp(argv[2], "8")));
        id[15] = argv[2][0] - '0';
    }
    assert(a20_mcu_leaf_set_io(raw_send, raw_receive, NULL) == 0);
    assert(a20_mcu_leaf_set_agent(&agent) == 0);
    if (oneway)
        assert(a20_mcu_leaf_set_sink("events", 6, counted_sink, NULL) == 0);
    assert(argc == 1 || argc == 3 || (argc == 2 && !strcmp(argv[1], "300")));
    assert(a20_mcu_leaf_configure(id, argc == 2 ? 300 : 200) == 0);
    fprintf(stderr, "LEAF-READY\n");
    uint64_t last_status = 0;
    for (;;) {
        a20_mcu_leaf_poll();
        uint64_t now = timer_get_ticks();
        if (now - last_status >= 1000) {
            fprintf(stderr,
                    "LEAF-STATUS state=%u txid=%u ack=%d drops=%llu "
                    "malformed=%llu dedup=%llu txdrops=%llu\n",
                    leaf_state, leaf_inflight_txid, leaf_reply_acknowledged,
                    (unsigned long long)leaf_c.rx_drops,
                    (unsigned long long)leaf_c.rx_malformed,
                    (unsigned long long)leaf_c.dedup_drops,
                    (unsigned long long)leaf_c.tx_drops);
            last_status = now;
        }
        struct timespec delay = {0, 1000000};
        nanosleep(&delay, NULL);
    }
}

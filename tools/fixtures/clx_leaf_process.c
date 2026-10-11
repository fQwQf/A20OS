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
static uint32_t counted_dot(void *ctx, const a20_work_request_t *request,
                            uint8_t *output, uint32_t *output_len) {
    fprintf(stderr, "DOT-EXEC:%u job=%llu shard=%u attempt=%u\n", ++executions,
            (unsigned long long)request->job, request->shard, request->attempt);
    return a20_work_dot(ctx, request, output, output_len);
}
int main(void) {
    assert(fcntl(STDIN_FILENO, F_SETFL, O_NONBLOCK) == 0);
    a20_mcu_leaf_init();
    a20_work_operator_t ops[] = {
        {A20_WORK_DOT, A20_WORK_REPLAY_SAFE, 140, 8, counted_dot, NULL},
        {A20_WORK_SUM64, A20_WORK_REPLAY_SAFE | A20_WORK_ASSOCIATIVE, 136, 8,
         a20_work_sum64, NULL},
    };
    a20_work_agent_t agent = leaf_default_agent;
    agent.operators = ops;
    uint8_t id[16] = {0};
    id[15] = 2;
    assert(a20_mcu_leaf_set_io(raw_send, raw_receive, NULL) == 0);
    assert(a20_mcu_leaf_set_agent(&agent) == 0);
    assert(a20_mcu_leaf_configure(id, 200) == 0);
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

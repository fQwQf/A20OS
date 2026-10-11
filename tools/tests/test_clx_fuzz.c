/* Deterministic, sanitizer-friendly corpus mutation of the shipped decoder
 * and firmware dispatch, including valid CRCs to reach deep parsers. */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CONFIG_MCU 1
#define CONFIG_CLUSTER_PROFILE 1
#define _TIMER_H
#define TICKS_PER_SEC 1000u
#define MS_TO_TICKS(ms) ((uint64_t)(ms))
static uint64_t ticks, transmitted;
uint64_t timer_get_ticks(void) {
    return ticks;
}
void uart_putc(char c) {
    (void)c;
    transmitted++;
}
int uart_try_getc(void) {
    return -1;
}
int klog_level = 4;
void klog_write(const char *format, ...) {
    (void)format;
}
#define printf a20_fuzz_kernel_printf
#include "../../kernel/cluster/frame.c"
#include "../../kernel/mcu/leaf.c"
#undef printf
static uint32_t random_state = 0x20c1a55u;
static uint32_t next_random(void) {
    uint32_t x = random_state;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return random_state = x;
}
int main(void) {
    uint8_t head[16] = {1}, node[16] = {2}, payload[224], frame[512];
    a20_mcu_leaf_init();
    assert(a20_mcu_leaf_configure(node, 200) == 0);
    uint32_t accepted = 0;
    for (unsigned i = 0; i < 1000000; i++) {
        ticks++;
        uint32_t type = 1 + i % 11, len = 0;
        memset(payload, 0, sizeof(payload));
        if (type == A20_CLX_TYPE_HELLO || type == A20_CLX_TYPE_HELLO_ACK) {
            a20_clx_build_hello_payload(head, 0, 1, 2, A20_CLX_CAP_RELIABLE, 7,
                                        100, payload);
            len = 32;
        } else if (type == A20_CLX_TYPE_PING || type == A20_CLX_TYPE_PONG ||
                   type == A20_CLX_TYPE_ERROR) {
            a20_clx_put_le64(payload, ticks);
            len = 8;
        } else if (type == A20_CLX_TYPE_CALL ||
                   type == A20_CLX_TYPE_CALL_REPLY ||
                   type == A20_CLX_TYPE_SEND) {
            a20_work_request_t work = {.kind = A20_WORK_QUERY};
            len = a20_work_encode(payload, sizeof(payload), &work);
        } else if (type == A20_CLX_TYPE_ACK || type == A20_CLX_TYPE_NACK) {
            len = 4;
        }
        int n = a20_clx_build_frame(
            frame, sizeof(frame), type, 0, 0,
            type == A20_CLX_TYPE_CALL || type == A20_CLX_TYPE_CALL_REPLY ? i + 1
                                                                         : 0,
            a20_clx_fnv1a32(head), a20_clx_fnv1a32(node),
            type == A20_CLX_TYPE_CALL ? A20_MCU_LEAF_WORK_SLOT : 0, payload,
            len);
        assert(n >= 34);
        unsigned mode = i % 8;
        if (mode == 1)
            n = next_random() % (unsigned)n;
        else if (mode == 2) {
            unsigned extra = next_random() % 100;
            memset(frame + n, 0xa5, extra);
            n += extra;
        } else if (mode >= 3) {
            for (unsigned bit = 0; bit < 1 + mode; bit++) {
                unsigned at = next_random() % (unsigned)n;
                frame[at] ^= 1u << (next_random() % 8);
            }
            if (mode & 1)
                a20_clx_put_le16(frame + n - 2,
                                 a20_clx_crc16_ccitt(frame, n - 2));
        }
        if (mode == 6) {
            n = 34 + next_random() % 32;
            frame[A20_CLX_OFF_TYPE] =
                i & 1 ? A20_CLX_TYPE_HELLO : A20_CLX_TYPE_HELLO_ACK;
            a20_clx_put_le16(frame + A20_CLX_OFF_PAYLOAD_LEN, n - 34);
            a20_clx_put_le16(frame + n - 2, a20_clx_crc16_ccitt(frame, n - 2));
        }
        /* Exact-size allocations turn reads past actual input into ASan
         * errors instead of silently reading stale bytes in the 512B arena. */
        uint8_t *exact = malloc(n ? (unsigned)n : 1u);
        assert(exact);
        memcpy(exact, frame, n);
        a20_frame_hdr_t decoded;
        const uint8_t *body;
        a20_frame_verdict_t verdict =
            a20_frame_decode_session(exact, n, 256, &decoded, &body);
        if (verdict == A20_FRAME_OK) {
            accepted++;
            assert(body >= exact && body <= exact + n &&
                   decoded.payload_len <= (uint32_t)(exact + n - body));
        }
        a20_mcu_leaf_on_frame(exact, n);
        free(exact);
    }
    assert(accepted > 100000 && transmitted > 0 && leaf_c.rx_frames > 1000);
    printf("test_clx_fuzz: 1000000 mutations, %u decoded, firmware dispatch "
           "bounded; passed\n",
           accepted);
}

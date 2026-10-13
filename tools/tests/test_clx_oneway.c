/* Reuse the byte-stream/timer seams, executing both production UART peers.
 * The inherited suite remains a separate test; this main targets SEND. */
#define main clx_existing_uart_pair_suite_main
#include "test_clx_uart_pair.c"
#undef main

static unsigned sink_calls, sink_bytes;
static uint32_t sink_status;
static uint32_t accept_message(void *ctx, const uint8_t *p, uint32_t len) {
    assert(ctx == &sink_calls && len <= 206);
    for (unsigned i = 0; i < len; i++)
        assert(p[i] == (uint8_t)(i * 73));
    sink_calls++;
    sink_bytes += len;
    return sink_status;
}
static int make_request(uint8_t *frame, const uint8_t *source, uint64_t epoch,
                        uint32_t seq, uint32_t slot, unsigned bytes) {
    uint8_t payload[222];
    a20_clx_session_encode(payload, epoch, leaf_nonce);
    for (unsigned i = 0; i < bytes; i++)
        payload[16 + i] = (uint8_t)(i * 73);
    int n = a20_clx_build_frame(
        frame, 256, A20_CLX_TYPE_SEND, A20_CLX_FLAG_RELIABLE, 0, 0,
        a20_clx_fnv1a32(source), a20_clx_fnv1a32(leaf_self_id), slot, payload,
        bytes + 16);
    assert(n > 0);
    leaf_set_reliable_seq(frame, n, seq);
    return n;
}
static void retire(const uint8_t *source, uint64_t epoch, uint32_t seq) {
    uint8_t payload[16], frame[64];
    a20_clx_session_encode(payload, epoch, leaf_nonce);
    int n = a20_clx_build_frame(
        frame, sizeof(frame), A20_CLX_TYPE_CALL, A20_CLX_FLAG_RELIABLE, 0, seq,
        a20_clx_fnv1a32(source), a20_clx_fnv1a32(leaf_self_id), 0, payload, 16);
    leaf_set_reliable_seq(frame, n, seq);
    inject_frame(&to_leaf, frame, n);
    pump(100);
}
int main(void) {
    uint8_t frame[256];
    a20_mcu_leaf_init();
    assert(!(leaf_caps() & A20_CLX_CAP_ONEWAY));
    assert(a20_mcu_leaf_set_sink("dot", 3, accept_message, &sink_calls) < 0);
    assert(a20_mcu_leaf_set_sink("events", 6, accept_message, &sink_calls) ==
           0);
    assert(a20_mcu_leaf_set_io(leaf_raw_putc, leaf_raw_getc, &to_leaf) == 0);
    uint8_t leaf_id[16] = {0};
    leaf_id[15] = 2;
    assert(a20_mcu_leaf_configure(leaf_id, 200) == 0);
    a20_clx_uart_link_cfg_t cfg = {0};
    cfg.self_node_id[15] = 1;
    cfg.assigned_addr = 7;
    cfg.session = 100;
    cfg.caps = A20_CLX_CAP_RELAY | A20_CLX_CAP_RELIABLE | A20_CLX_CAP_ONEWAY;
    cfg.drv.getc = head_getc;
    cfg.drv.putc = head_putc;
    assert(a20_clx_uart_link_register(&cfg) >= 0);
    a20_clx_uart_set_rx_handler(on_rx, NULL);
    a20_clx_uart_set_link_event_handler(on_link, NULL);
    pump(3000);
    assert(link_up && (leaf_peer_caps & A20_CLX_CAP_ONEWAY));
    assert(a20_mcu_leaf_set_sink("late", 4, accept_message, &sink_calls) < 0);
    uint64_t local, remote;
    uint32_t caps;
    uint8_t nh[2] = {7, 0};
    assert(!a20_clx_uart_session(nh, 2, leaf_id, &local, &remote, &caps));
    assert(local == 100 && remote == 200 && (caps & A20_CLX_CAP_ONEWAY));
    uint8_t directory_request[16], directory_reply[128];
    a20_directory_request(directory_request, 2, 1);
    assert(leaf_directory(directory_request, 16, directory_reply) == 128);
    a20_directory_record_t record;
    assert(!a20_directory_decode(directory_reply, 128, leaf_id, &record));
    assert(record.slot == 3 && record.flags == A20_DIRECTORY_ONEWAY &&
           record.name_len == 6 && !memcmp(record.name, "events", 6));
    unsigned total = 0;
    uint32_t seq = UINT32_MAX - 2;
    for (unsigned i = 0; i < 256; i++) {
        if (!seq || seq == A20_CLX_CLOSE_CALLER)
            seq = 1;
        const unsigned sizes[] = {0, 1, 64, 206};
        unsigned size = sizes[i % 4];
        int n = make_request(frame, cfg.self_node_id, local, seq, 3, size);
        assert(!a20_clx_uart_send_frame(nh, 2, frame, n));
        pump(100);
        total += size;
        assert(sink_calls == i + 1 && sink_bytes == total);
        assert(received_len == 54 && received[3] == A20_CLX_TYPE_CALL_REPLY &&
               a20_clx_get_le32(received + 8) == seq &&
               a20_clx_get_le32(received + 48) == 0);
        for (unsigned duplicate = 0; duplicate < 3; duplicate++) {
            assert(!a20_clx_uart_send_frame(nh, 2, frame, n));
            pump(100);
            assert(sink_calls == i + 1 && received_len == 54);
        }
        if (!i) {
            int full =
                make_request(frame, cfg.self_node_id, local, seq + 1, 3, 64);
            assert(!a20_clx_uart_send_frame(nh, 2, frame, full));
            pump(100);
            assert(sink_calls == 1 && a20_clx_get_le32(received + 48) == 13);
        }
        retire(cfg.self_node_id, local, seq);
        seq++;
    }
    int n = make_request(frame, cfg.self_node_id, local - 1, seq, 3, 64);
    assert(!a20_clx_uart_send_frame(nh, 2, frame, n));
    pump(100);
    assert(sink_calls == 256); /* Previous boot cannot execute. */
    n = make_request(frame, cfg.self_node_id, local, seq, 2, 64);
    assert(!a20_clx_uart_send_frame(nh, 2, frame, n));
    pump(100);
    assert(sink_calls == 256 &&
           a20_clx_get_le32(received + 48) == A20_CLX_WERR_NOT_FOUND);
    retire(cfg.self_node_id, local, seq++);
    /* An admitted callback error is cached too: no repeated side effects. */
    sink_status = 13;
    n = make_request(frame, cfg.self_node_id, local, seq, 3, 1);
    for (unsigned i = 0; i < 3; i++) {
        assert(!a20_clx_uart_send_frame(nh, 2, frame, n));
        pump(100);
        assert(sink_calls == 257 && a20_clx_get_le32(received + 48) == 13);
    }
    retire(cfg.self_node_id, local, seq++);
    sink_status = 0;
    /* Retirement can overtake a request; it must never reach the callback. */
    retire(cfg.self_node_id, local, seq);
    n = make_request(frame, cfg.self_node_id, local, seq++, 3, 64);
    assert(!a20_clx_uart_send_frame(nh, 2, frame, n));
    pump(100);
    assert(sink_calls == 257 && a20_clx_get_le32(received + 48) == 20);
    n = make_request(frame, cfg.self_node_id, local, seq, 3, 64);
    a20_clx_put_le32(frame + A20_CLX_OFF_TXID, seq);
    leaf_set_reliable_seq(frame, n, seq);
    inject_frame(&to_leaf, frame, n);
    pump(100);
    assert(sink_calls == 257); /* SEND never carries a wire transaction id. */
    /* A routed sender negotiates its own epoch and capability via real CS. */
    uint8_t caller[16] = {0};
    caller[15] = 99;
    routed_confirmation(caller, 500, NULL, NULL);
    n = make_request(frame, caller, 500, 1, 3, 206);
    inject_frame(&to_leaf, frame, n);
    pump(100);
    inject_frame(&to_leaf, frame, n);
    pump(100);
    assert(sink_calls == 258 && leaf_reply_origin_epoch == 500);
    retire(caller, 500, 1);
    puts("test_clx_oneway: real UART peers, 256 SENDs, 768 duplicates, wrap, "
         "retirement, cached errors, directory, old epoch and routed CS PASS");
}

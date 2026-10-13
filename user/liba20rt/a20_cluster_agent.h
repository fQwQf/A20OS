#ifndef A20_CLUSTER_AGENT_H
#define A20_CLUSTER_AGENT_H
#include "a20_sdk.h"
#include "../../kernel/include/uapi/a20/cluster_work.h"

#define A20_AGENT_LOCAL_CONNECTIONS 8u
#define A20_AGENT_BUFFER 1056u
typedef struct a20_cluster_agent_server {
    a20_handle_t exported;
    a20_handle_t local[A20_AGENT_LOCAL_CONNECTIONS];
    a20_work_agent_t agent;
    int (*dispatch)(void *ctx, const uint8_t *input, uint32_t len,
                    uint8_t *reply, uint32_t capacity);
    void *dispatch_ctx;
} a20_cluster_agent_server_t;

static inline void a20_cluster_agent_init(a20_cluster_agent_server_t *server,
                                          a20_handle_t endpoint,
                                          const a20_work_agent_t *agent) {
    server->exported = endpoint;
    server->agent = *agent;
    server->dispatch = 0;
    server->dispatch_ctx = 0;
    for (unsigned i = 0; i < A20_AGENT_LOCAL_CONNECTIONS; i++)
        server->local[i] = A20_HANDLE_NULL;
}

static inline uint64_t a20_agent_clock_ms(void *ctx) {
    (void)ctx;
    a20_time_ns_t now = 0;
    a20_clock_get(A20_CLOCK_MONOTONIC, &now);
    return now / 1000000;
}

/* Fair, bounded nonblocking drain. Remote requests stay on the export FIFO;
 * LOCAL connect delivers a separate endpoint handle. Never consume a local
 * reply through the remote export's reply worker. Return count or error. */
static inline int a20_cluster_agent_pump(a20_cluster_agent_server_t *server) {
    uint8_t input[A20_AGENT_BUFFER], reply[A20_AGENT_BUFFER];
    int handled = 0;
    for (unsigned i = 0; i <= A20_AGENT_LOCAL_CONNECTIONS; i++) {
        a20_handle_t ep = i ? server->local[i - 1] : server->exported;
        if (ep == A20_HANDLE_NULL)
            continue;
        a20_handle_t incoming = A20_HANDLE_NULL;
        uint32_t len = sizeof(input), count = 1;
        a20_status_t r = a20_channel_recv_flags(ep, input, &len, &incoming,
                                                &count, A20_MSG_NONBLOCK);
        if (r == -A20_ERR_WOULD_BLOCK)
            continue;
        if (r < 0) {
            if (!i)
                return (int)r;
            a20_hdl_close(ep);
            server->local[i - 1] = A20_HANDLE_NULL;
            continue;
        }
        handled++;
        if (count) {
            if (!i && !len) {
                for (unsigned j = 0; j < A20_AGENT_LOCAL_CONNECTIONS; j++) {
                    if (server->local[j] == A20_HANDLE_NULL) {
                        server->local[j] = incoming;
                        incoming = A20_HANDLE_NULL;
                        break;
                    }
                }
            }
            if (incoming != A20_HANDLE_NULL)
                a20_hdl_close(incoming);
            continue;
        }
        int n = server->dispatch ? server->dispatch(server->dispatch_ctx, input,
                                                    len, reply, sizeof(reply))
                                 : a20_work_dispatch(&server->agent, input, len,
                                                     reply, sizeof(reply));
        r = n < 0
                ? -A20_ERR_INVALID_ARGUMENT
                : a20_channel_send_flags(ep, reply, n, 0, 0, A20_MSG_NONBLOCK);
        if (r < 0) {
            if (!i) {
                /* Terminate the FIFO, so pending calls receive an error
                 * instead of having subsequent replies mispaired. */
                a20_hdl_close(server->exported);
                server->exported = A20_HANDLE_NULL;
                return (int)r;
            }
            a20_hdl_close(ep);
            server->local[i - 1] = A20_HANDLE_NULL;
        }
    }
    return handled;
}

#endif

/*
 * echod — demo user-space service for the hybrid-kernel supervisor.
 *
 * Receives versioned IDL requests on its service channel endpoint
 * (installed at A20_SVC_ENDPOINT_SLOT by svcman's task_spawn):
 * SVCMGR_REQ_ECHO is echoed back verbatim, SVCMGR_REQ_CRASH makes the
 * service exit with A20_SVC_CRASH_CODE so svcman can demonstrate
 * detection + restart (docs/hybrid-kernel/01-roadmap.md phase 2).
 */
#include "liba20rt/a20_sdk.h"
#include "a20_services_idl.h"
#include "liba20rt/crt0_a20.h"

/* Drain one endpoint non-blockingly.  Returns 1 if a message was
 * handled, 0 if idle, -1 if the (serve) endpoint is gone. */
static int pump(a20_handle_t ep, int is_serve)
{
    uint8_t buf[64];
    uint32_t blen = sizeof(buf);
    uint32_t hcnt = 0;
    a20_status_t st =
        a20_channel_recv_flags(ep, buf, &blen, 0, &hcnt, A20_MSG_NONBLOCK);
    if (st == -A20_ERR_WOULD_BLOCK)
        return 0;
    if (st < 0)
        return is_serve ? -1 : 0; /* ping slot may be absent (standalone) */
    if (blen < sizeof(a20_idl_envelope_t))
        return 1;
    a20_idl_envelope_t env;
    a20_memcpy(&env, buf, sizeof(env));
    if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
        return 1; /* protocol mismatch: ignore, do not crash */
    if (env.type == SVCMGR_REQ_CRASH)
        return 42;
    if (a20_channel_send(ep, buf, blen, 0, 0) < 0)
        return is_serve ? -1 : 0;
    return 1;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* 服务端点 + 监管 ping 端点双通道轮询（ping 通道隔离健康探测
     * 流量，见 a20_services_idl.h A20_SVC_PING_SLOT）。 */
    a20_handle_t ep = ((a20_handle_t)A20_SVC_ENDPOINT_SLOT);
    a20_handle_t ping_ep = ((a20_handle_t)A20_SVC_PING_SLOT);
    for (;;) {
        int idle = 1;
        for (;;) {
            int r = pump(ping_ep, 0);
            if (r <= 0)
                break;
            idle = 0;
        }
        for (;;) {
            int r = pump(ep, 1);
            if (r == 42)
                return A20_SVC_CRASH_CODE;
            if (r < 0)
                return 0; /* peer closed or handle revoked: clean shutdown */
            if (r == 0)
                break;
            idle = 0;
        }
        if (idle) {
            a20_time_t nap = { .secs = 0, .nsecs = 1000 * 1000 };
            a20_thread_sleep(nap);
        }
    }
}

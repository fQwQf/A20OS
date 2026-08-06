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
#include "liba20rt/crt0_a20.h"
#include "../svc/svc_proto.h"

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_handle_t ep = A20_SVC_ENDPOINT_HANDLE;
    for (;;) {
        uint8_t buf[64];
        uint32_t blen = sizeof(buf);
        uint32_t hcnt = 0;
        a20_status_t st = a20_channel_recv(ep, buf, &blen, 0, &hcnt);
        if (st < 0)
            return 0; /* peer closed or handle revoked: clean shutdown */
        if (blen < sizeof(a20_idl_envelope_t))
            continue;
        a20_idl_envelope_t env;
        a20_memcpy(&env, buf, sizeof(env));
        if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
            continue; /* protocol mismatch: ignore, do not crash */
        if (env.type == SVCMGR_REQ_CRASH)
            return A20_SVC_CRASH_CODE;
        if (a20_channel_send(ep, buf, blen, 0, 0) < 0)
            return 1;
    }
}

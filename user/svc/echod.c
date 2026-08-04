/*
 * echod — demo user-space service for the hybrid-kernel supervisor.
 *
 * Receives requests on its service channel endpoint (installed at
 * A20_SVC_ENDPOINT_SLOT by svcman's task_spawn), echoes payloads back,
 * and deliberately exits on the "crash" request so svcman can demonstrate
 * detection + restart (docs/hybrid-kernel/01-roadmap.md phase 2).
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/svc_proto.h"

static int is_crash(const uint8_t *buf, uint32_t len)
{
    static const char k[] = { 'c', 'r', 'a', 's', 'h' };
    if (len != 5) return 0;
    for (uint32_t i = 0; i < 5; i++)
        if (buf[i] != (uint8_t)k[i]) return 0;
    return 1;
}

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
        if (is_crash(buf, blen))
            return A20_SVC_CRASH_CODE;
        if (a20_channel_send(ep, buf, blen, 0, 0) < 0)
            return 1;
    }
}

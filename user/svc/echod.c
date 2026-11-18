/*
 * echod — demo user-space service for the hybrid-kernel supervisor.
 *
 * Receives versioned IDL requests on its service channel endpoint
 * (installed at A20_SVC_ENDPOINT_SLOT by svcman's task_spawn):
 * SVCMGR_REQ_ECHO is echoed back verbatim, SVCMGR_REQ_CRASH makes the
 * service exit with A20_SVC_CRASH_CODE so svcman can demonstrate
 * detection + restart (docs/zh/hybrid-kernel/01-roadmap.md phase 2), and
 * SVCMGR_REQ_HANG parks the service until the supervisor's health
 * deadline force-exits it (supervision self-test, same class as CRASH).
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
    if (blen < A20_IDL_ENVELOPE_WIRE_SIZE)
        return 1;
    a20_idl_envelope_t env;
    a20_idl_envelope_decode(&env, buf);
    if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
        return 1; /* protocol mismatch: ignore, do not crash */
    if (env.type == SVCMGR_REQ_CRASH)
        return 42;
    if (env.type == SVCMGR_REQ_HANG)
        return 43; /* serve loop parks; the supervisor force-exits it */
    if (env.type == SVCMGR_REQ_VERSION_NEGOTIATE) {
        if (blen != A20_IDL_ENVELOPE_WIRE_SIZE +
                    A20_IDL_SVCMGR_VERSION_REQUEST_WIRE_SIZE)
            return 1;

        a20_idl_svcmgr_version_request_t request;
        a20_idl_svcmgr_version_request_decode(
            &request, buf + A20_IDL_ENVELOPE_WIRE_SIZE);

        a20_idl_svcmgr_version_response_t response = {
            .min_version = 1,
            .max_version = 1,
            .selected_version = 0,
            .status = SVCMGR_VERSION_NEGOTIATE_OK,
            .nonce = request.nonce,
        };
        if (request.min_version == 0 ||
            request.min_version > request.max_version) {
            response.status = SVCMGR_VERSION_NEGOTIATE_INVALID_RANGE;
        } else {
            uint16_t common_min = request.min_version > response.min_version
                ? request.min_version : response.min_version;
            uint16_t common_max = request.max_version < response.max_version
                ? request.max_version : response.max_version;
            if (common_min > common_max) {
                response.status = SVCMGR_VERSION_NEGOTIATE_NO_COMMON;
            } else {
                /* Select the highest version both peers advertise. */
                response.selected_version = common_max;
            }
        }

        uint8_t reply[A20_IDL_ENVELOPE_WIRE_SIZE +
                      A20_IDL_SVCMGR_VERSION_RESPONSE_WIRE_SIZE];
        a20_idl_envelope_t reply_env = {
            .version = A20_SERVICES_IDL_VERSION,
            .type = SVCMGR_REP_VERSION_NEGOTIATE,
            .size = sizeof(reply),
        };
        a20_idl_envelope_encode(reply, &reply_env);
        a20_idl_svcmgr_version_response_encode(
            reply + A20_IDL_ENVELOPE_WIRE_SIZE, &response);
        if (a20_channel_send(ep, reply, sizeof(reply), 0, 0) < 0)
            return is_serve ? -1 : 0;
        return 1;
    }
    if (a20_channel_send(ep, buf, blen, 0, 0) < 0)
        return is_serve ? -1 : 0;
    return 1;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    /* Poll both the server endpoint and the supervision ping endpoint on two
     * channels (the ping channel isolates health probe traffic, see
     * A20_SVC_PING_SLOT in a20_services_idl.h). */
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
            if (r == 43) {
                /* Supervision self-test (SVCMGR_REQ_HANG): stop servicing
                 * both channels.  The supervisor observes the silent health
                 * channel and force-exits this task; the sleep loop keeps
                 * the task off the runqueue and consumes the pending exit
                 * at the next syscall boundary. */
                for (;;) {
                    a20_time_t nap = { .secs = 0, .nsecs = 1000 * 1000 };
                    a20_thread_sleep(nap);
                }
            }
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

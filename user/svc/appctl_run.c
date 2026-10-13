/* Run one configured application using the portable cluster launcher. The
 * calling task stays alive, retaining the ticket identity until real exit. */
#include "liba20rt/a20_apps.h"
#include "liba20rt/crt0_a20.h"
static a20_handle_t output = A20_HANDLE_NULL;
static void text(const char *s) {
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, s, a20_strlen(s), 0);
}
static void hex(uint64_t v, unsigned digits) {
    char b[16];
    for (unsigned i = 0; i < digits; i++)
        b[digits - 1 - i] = "0123456789abcdef"[(v >> (4 * i)) & 15];
    if (output != A20_HANDLE_NULL)
        a20_hdl_write_buf(output, b, digits, 0);
}
static int decimal(const char *s, uint64_t *out) {
    uint64_t v = 0;
    if (!s || !*s)
        return -1;
    for (; *s; s++) {
        if (*s < '0' || *s > '9' ||
            v > (UINT64_MAX - (unsigned)(*s - '0')) / 10)
            return -1;
        v = v * 10 + (unsigned)(*s - '0');
    }
    *out = v;
    return 0;
}
static void retire(a20_app_session_t *session, const a20_app_ticket_t *ticket) {
    uint64_t deadline = a20_agent_clock_ms(0) + 8000;
    while (a20_agent_clock_ms(0) < deadline) {
        if (session->endpoint == A20_HANDLE_NULL) {
            a20_status_t status = a20_app_resume(ticket, 2000, session);
            if (status == -A20_ERR_EXPIRED)
                return;
            if (status < 0)
                continue;
        }
        a20_app_result_t result;
        a20_status_t status = a20_app_release(session, ticket, 2000, &result);
        if (status >= 0 || status == -A20_ERR_EXPIRED)
            return;
    }
    text("APPCTL: terminal result observed; slot retirement unconfirmed\n");
}
int main(int argc, char **argv, char **envp) {
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    if (si)
        output = si->stdout_handle;
    uint64_t nonce, budget;
    a20_node_id_t manifest, domain;
    uint8_t domains[16][16];
    if (argc < 5 || argc > 20 || a20_node_id_parse_hex(argv[1], &manifest) ||
        !a20_runtime_id_valid(manifest.bytes) || decimal(argv[2], &nonce) ||
        !nonce || decimal(argv[3], &budget) || !budget || budget > 30000) {
        text("usage: appctl-run UUID NONCE START-BUDGET-MS DOMAIN-UUID...\n");
        return 64;
    }
    for (int i = 4; i < argc; i++) {
        if (a20_node_id_parse_hex(argv[i], &domain) ||
            !a20_runtime_id_valid(domain.bytes))
            return 64;
        a20_memcpy(domains[i - 4], domain.bytes, 16);
    }
    a20_app_ticket_t ticket = {0};
    a20_app_result_t result;
    a20_status_t status =
        a20_app_start_any_reusable(domains, (unsigned)argc - 4, manifest.bytes,
                                   nonce, (uint32_t)budget, &ticket, &result);
    if (!ticket.nonce) {
        text("APPCTL: no launch accepted\n");
        return 125;
    }
    text("APPCTL: owner=");
    for (unsigned i = 0; i < 16; i++)
        hex(ticket.controller.domain_id[i], 2);
    text(" controller=");
    hex(ticket.controller.serial, 16);
    text(" client=");
    hex(ticket.client.serial, 16);
    text(" token=");
    hex(ticket.token, 16);
    text(" nonce=");
    hex(ticket.nonce, 16);
    text("\n");
    /* Even START timeout is reconciled only at the recorded owner. There is
     * never a second placement after a potentially accepted launch. */
    a20_app_session_t session = {.endpoint = A20_HANDLE_NULL};
    uint64_t recovery = a20_agent_clock_ms(0);
    unsigned cancelled = 0, reported_root = 0;
    for (;;) {
        if (a20_agent_clock_ms(0) - recovery >= 12000) {
            a20_app_close(&session);
            text("APPCTL: outcome unknown; original owner retained\n");
            return 125;
        }
        if (session.endpoint == A20_HANDLE_NULL) {
            status = a20_app_resume(&ticket, 4000, &session);
            if (status < 0) {
                if (status == -A20_ERR_EXPIRED ||
                    a20_agent_clock_ms(0) - recovery >= 12000) {
                    text("APPCTL: outcome unknown; original owner retained\n");
                    return 125;
                }
                a20_thread_sleep((a20_time_t){.nsecs = 50000000});
                continue;
            }
        }
        status = a20_app_query(&session, &ticket, 4000, &result);
        if (status < 0) {
            if (status == -A20_ERR_NOT_FOUND || status == -A20_ERR_EXPIRED) {
                text("APPCTL: launch absent or expired\n");
                return 125;
            }
            continue;
        }
        recovery = a20_agent_clock_ms(0);
        if (!reported_root && a20_object_id_valid(&result.root)) {
            text("APPCTL: root=");
            for (unsigned i = 0; i < 16; i++)
                hex(result.root.domain_id[i], 2);
            text("/");
            hex(result.root.serial, 16);
            text("\n");
            reported_root = 1;
        }
        if (result.state == A20_APP_EXITED) {
            int32_t code = result.exit_code;
            retire(&session, &ticket);
            a20_app_close(&session);
            text("APPCTL: exit=");
            hex((uint32_t)code, 8);
            text("\n");
            return code;
        }
        if (result.state == A20_APP_FAILED) {
            retire(&session, &ticket);
            a20_app_close(&session);
            text("APPCTL: image launch failed\n");
            return 125;
        }
        uint64_t signals = a20_signal_check();
        if (!cancelled && (signals & ((1ULL << 2) | (1ULL << 15))))
            cancelled = (signals & (1ULL << 2)) ? 2 : 15;
        if (cancelled && result.state != A20_APP_STOPPING) {
            if (a20_app_stop(&session, &ticket, 9, 4000, &result) < 0)
                text("APPCTL: stop retry original owner\n");
        }
        a20_thread_sleep((a20_time_t){.nsecs = 50000000});
    }
}

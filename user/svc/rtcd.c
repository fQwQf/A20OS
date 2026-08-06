/*
 * rtcd — user-space goldfish-RTC driver service (hybrid-kernel phase 4,
 * dual-placement user shell).
 *
 * Owns the QEMU virt goldfish RTC entirely from user space: the kernel
 * only mapped the whitelisted MMIO window and routes IRQ 11 to this
 * task's event queue.  Requests arrive on the service channel endpoint
 * (installed at A20_RTCD_EP_SLOT by the supervisor).
 *
 * The device register protocol is shared verbatim with the kernel
 * placement (kernel/drivers/char/goldfish_rtc_kdrv.c) through
 * kernel/include/drivers/dual/goldfish_rtc.h — only the shell differs
 * (this main loop + EventQ vs kernel init/ISR).
 */
#define DRV_ENV_USER 1
#include "drivers/dual/goldfish_rtc.h"
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/rtcd_proto.h"

#define IRQ_TAG 0x52435449ULL /* "RTCI" — event user_data for the irq */
#define CH_TAG  0x52435443ULL /* "RTCC" — event user_data for the channel */

static uint64_t g_rtc;

static uint64_t rtc_read_ns(void)
{
    return grtc_read_ns(g_rtc);
}

static void rtc_set_alarm_ns(uint64_t ns)
{
    grtc_set_alarm_ns(g_rtc, ns);
}

/* Returns 1 = handled one message, 0 = queue empty, 42 = crash request,
 * -1 = peer gone (clean shutdown). */
static int handle_one(a20_handle_t ep, int *alarm_pending)
{
    uint8_t buf[64];
    uint32_t blen = sizeof(buf);
    uint32_t hcnt = 0;
    a20_status_t st = a20_channel_recv_flags(ep, buf, &blen, 0, &hcnt,
                                             A20_MSG_NONBLOCK);
    if (st == -A20_ERR_WOULD_BLOCK)
        return 0;
    if (st < 0)
        return -1;
    if (blen < sizeof(a20_idl_envelope_t))
        return 1;

    a20_idl_envelope_t env;
    a20_memcpy(&env, buf, sizeof(env));
    if (env.version != A20_SERVICES_IDL_VERSION || env.size != blen)
        return 1;

    switch (env.type) {
    case RTCD_REQ_TIME: {
        uint64_t ns = rtc_read_ns();
        struct {
            a20_idl_envelope_t env;
            a20_idl_rtcd_time_response_t body;
        } rep = {
            { A20_SERVICES_IDL_VERSION, RTCD_REPLY_TIME,
              sizeof(a20_idl_envelope_t) + sizeof(a20_idl_rtcd_time_response_t) },
            {
            ns / 1000000000ULL, ns % 1000000000ULL
            }
        };
        a20_channel_send(ep, &rep, sizeof(rep), 0, 0);
        return 1;
    }
    case RTCD_REQ_ALARM: {
        if (blen != sizeof(a20_idl_envelope_t) + sizeof(a20_idl_rtcd_alarm_request_t))
            return 1;
        a20_idl_rtcd_alarm_request_t req;
        a20_memcpy(&req, &buf[sizeof(a20_idl_envelope_t)], sizeof(req));
        uint32_t ms = req.milliseconds;
        rtc_set_alarm_ns(rtc_read_ns() + (uint64_t)ms * 1000000ULL);
        *alarm_pending = 1; /* async reply is sent when the IRQ fires */
        return 1;
    }
    case RTCD_REQ_CRASH:
        return 42;
    default:
        return 1;
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    a20_handle_t out = si ? si->stdout_handle : A20_HANDLE_NULL;
#define RTCD_LOG(msg) a20_hdl_write_buf(out, msg, sizeof(msg) - 1, (void *)0)
    RTCD_LOG("rtcd: start\n");

    if (a20_device_claim(GOLDFISH_RTC_BASE) != A20_OK) {
        RTCD_LOG("rtcd: claim failed\n");
        return 2;
    }
    uint64_t rtc_va = grtc_map();
    if (!rtc_va) {
        RTCD_LOG("rtcd: map_mmio failed\n");
        return 2;
    }
    RTCD_LOG("rtcd: mmio mapped\n");
    g_rtc = rtc_va;

    a20_handle_t eq;
    if (a20_event_queue_create(&eq) != A20_OK)
        return 3;

    a20_handle_t ep = A20_RTCD_EP_HANDLE;
    if (a20_event_watch(eq, ep, A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                        CH_TAG) != A20_OK) {
        RTCD_LOG("rtcd: watch ep failed\n");
        return 4;
    }
    RTCD_LOG("rtcd: ep watched\n");
    if (a20_device_irq_listen(GOLDFISH_RTC_IRQ, eq, IRQ_TAG) != A20_OK) {
        RTCD_LOG("rtcd: irq_listen failed\n");
        return 5;
    }
    RTCD_LOG("rtcd: irq listening\n");

    int alarm_pending = 0;
    for (;;) {
        /* Drain before waiting: requests that arrived before the watch was
         * registered (supervisor respawn races) never generate a fresh
         * MESSAGE_READY event, so event-first waiting could miss them. */
        for (;;) {
            int r = handle_one(ep, &alarm_pending);
            if (r == 42)
                return 42;
            if (r <= 0)
                break;
        }

        a20_event_t ev;
        a20_time_t inf = { .secs = (uint64_t)-1, .nsecs = 0 };
        if (a20_event_wait(eq, inf, &ev) < 0)
            continue;

        if (ev.user_data == IRQ_TAG) {
            /* Alarm fired: clear the device IRQ, report, re-arm the line. */
            grtc_clear_alarm(g_rtc);
            if (alarm_pending) {
                uint64_t ns = rtc_read_ns();
                struct {
                    a20_idl_envelope_t env;
                    a20_idl_rtcd_time_response_t body;
                } rep = {
                    { A20_SERVICES_IDL_VERSION, RTCD_REPLY_ALARM,
                      sizeof(a20_idl_envelope_t) + sizeof(a20_idl_rtcd_time_response_t) },
                    { ns / 1000000000ULL, ns % 1000000000ULL }
                };
                a20_channel_send(ep, &rep, sizeof(rep), 0, 0);
                alarm_pending = 0;
            }
            a20_device_irq_ack(GOLDFISH_RTC_IRQ);
        }
        /* CH_TAG events are handled by the drain at the loop top. */
    }
}

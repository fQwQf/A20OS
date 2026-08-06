/* Native pipe-shaped personality PoC: channel + EventQ semantics. */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/a20_personality.h"
#include "liba20rt/crt0_a20.h"

static a20_handle_t out = A20_HANDLE_NULL;

static int fail(const char *s)
{
    if (out != A20_HANDLE_NULL) {
        a20_hdl_write_buf(out, "PERSONALITY: FAIL ", 19, NULL);
        a20_hdl_write_buf(out, s, a20_strlen(s), NULL);
        a20_hdl_write_buf(out, "\n", 1, NULL);
    }
    return 1;
}

static void report_status(const char *label, a20_status_t status)
{
    char buf[32];
    int n = 0;
    while (*label && n < 16)
        buf[n++] = *label++;
    buf[n++] = '=';
    if (status < 0) {
        buf[n++] = '-';
        status = -status;
    }
    char digits[12];
    int d = 0;
    do { digits[d++] = (char)('0' + status % 10); status /= 10; } while (status);
    while (d) buf[n++] = digits[--d];
    buf[n++] = '\n';
    a20_hdl_write_buf(out, buf, (uint64_t)n, NULL);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    out = si ? si->stdout_handle : A20_HANDLE_NULL;

    a20_personality_pipe_t pipe = {0};
    pipe.read_end = pipe.write_end = pipe.wait_queue = A20_HANDLE_NULL;
    if (a20_personality_pipe_create(&pipe) != A20_OK)
        return fail("create");

    const char msg[] = "native pipe payload";
    if (a20_personality_pipe_write(&pipe, msg, sizeof(msg) - 1) != A20_OK)
        return fail("write");

    a20_event_t ev;
    a20_time_t timeout = { .secs = 1, .nsecs = 0 };
    if (a20_personality_pipe_wait_readable(&pipe, timeout, &ev) != 1)
        return fail("readiness");
    if (!(ev.events & A20_EVENT_MASK(A20_EVENT_MESSAGE_READY)))
        return fail("event-kind");

    char buf[64] = {0};
    uint32_t len = 6;
    a20_status_t rr = a20_personality_pipe_read(&pipe, buf, &len);
    if (rr < 0) {
        report_status("read_status", rr);
        return fail("read");
    }
    if (rr != A20_OK) {
        report_status("read_status", rr);
        return fail("read-count");
    }
    if (len != 6 || a20_memcmp(buf, msg, len) != 0)
        return fail("partial-payload");
    len = sizeof(buf);
    if (a20_personality_pipe_read(&pipe, buf, &len) < 0)
        return fail("read-rest");
    if (len != sizeof(msg) - 1 - 6 ||
        a20_memcmp(buf, msg + 6, len) != 0)
        return fail("rest-payload");

    a20_personality_pipe_close(&pipe);
    if (out != A20_HANDLE_NULL)
        a20_hdl_write_buf(out, "NATIVE_PERSONALITY: PASS\n", 25, NULL);
    return 0;
}

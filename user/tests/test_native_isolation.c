/*
 * Isolation audit (docs/hybrid-kernel/02-mainstream-plan.md M2).
 *
 * Reads /proc/a20/objects, runs 100 spawn->crash->reap cycles of a
 * service, then re-reads the counters: every native object class
 * (handles, channel endpoints, event queues, VMOs, VMO pages, IRQ
 * bindings) must be back to the exact baseline.  Any delta is a leak in
 * the supervisor crash path.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/svc_proto.h"

#define AUDIT_CYCLES 100

static a20_handle_t g_out = A20_HANDLE_NULL;
static a20_handle_t g_root = A20_HANDLE_NULL;

static void put(const char *s, uint32_t len)
{
    a20_hdl_write_buf(g_out, s, len, (void *)0);
}

static void put_str(const char *s)
{
    uint32_t n = 0;
    while (s[n]) n++;
    put(s, n);
}

static void put_u64(uint64_t v)
{
    char buf[24];
    int i = 24;
    if (!v) buf[--i] = '0';
    while (v) { buf[--i] = (char)('0' + v % 10); v /= 10; }
    put(buf + i, (uint32_t)(24 - i));
}

static int fail(int code, const char *msg)
{
    put_str("NATIVE_ISOLATION: FAIL ");
    put_str(msg);
    put("\n", 1);
    return code;
}

/* ---- /proc/a20/objects parsing ---- */

typedef struct {
    uint64_t handles, channel_eps, eventqs, vmos, vmo_pages, irq_bindings;
} objstats_t;

static int objstats_read(objstats_t *out)
{
    a20_path_open_args_t oa;
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.flags = 0;
    oa.rights = A20_RIGHT_READ;
    static const char p[] = "/proc/a20/objects";
    oa.path = (uint64_t)(uintptr_t)p;
    oa.path_len = sizeof(p) - 1;
    oa.mode = 0;
    oa.out_handle = A20_HANDLE_NULL;
    if (a20_path_open(&oa) != A20_OK)
        return -1;

    char buf[512];
    uint32_t got = 0;
    for (;;) {
        uint64_t actual = 0;
        a20_status_t st = a20_hdl_read_buf(oa.out_handle, buf + got,
                                           sizeof(buf) - 1 - got, &actual);
        if (st < 0 || actual == 0)
            break;
        got += (uint32_t)actual;
        if (got >= sizeof(buf) - 1)
            break;
    }
    a20_hdl_close(oa.out_handle);
    buf[got] = 0;

    static const char *keys[6] = {
        "handles:", "channel_eps:", "eventqs:", "vmos:", "vmo_pages:",
        "irq_bindings:",
    };
    uint64_t *dst[6] = {
        &out->handles, &out->channel_eps, &out->eventqs, &out->vmos,
        &out->vmo_pages, &out->irq_bindings,
    };
    for (int k = 0; k < 6; k++) {
        /* find "key<space or nothing>digits" at line start */
        const char *s = buf;
        int found = 0;
        while (*s && !found) {
            const char *kp = keys[k];
            const char *sp = s;
            while (*kp && *sp == *kp) { kp++; sp++; }
            if (!*kp) {
                while (*sp == ' ') sp++;
                uint64_t v = 0;
                while (*sp >= '0' && *sp <= '9') {
                    v = v * 10 + (uint64_t)(*sp - '0');
                    sp++;
                }
                *dst[k] = v;
                found = 1;
            }
            while (*s && *s != '\n') s++;
            if (*s == '\n') s++;
        }
        if (!found)
            return -2;
    }
    return 0;
}

/* ---- one spawn->crash->reap cycle ---- */

static a20_status_t spawn_echod(a20_handle_t *out_ep, a20_handle_t *out_task)
{
    a20_channel_pair_t pair;
    a20_status_t st = a20_channel_create(&pair);
    if (st != A20_OK)
        return st;

    static const char path[] = "/bin/svc-echod-rv";
    a20_path_open_args_t oa;
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.flags = 0;
    oa.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    oa.path = (uint64_t)(uintptr_t)path;
    oa.path_len = sizeof(path) - 1;
    oa.mode = 0;
    oa.out_handle = A20_HANDLE_NULL;
    st = a20_path_open(&oa);
    if (st != A20_OK) {
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(pair.endpoints[1]);
        return st;
    }

    a20_spawn_handle_t sh;
    sh.handle = pair.endpoints[1];
    sh.rights = A20_RIGHT_READ | A20_RIGHT_WRITE;
    sh.target_slot = A20_SVC_ENDPOINT_SLOT;
    sh.flags = 0;

    a20_task_spawn_args_t ta;
    ta.size = sizeof(ta);
    ta.version = 2;
    ta.image = oa.out_handle;
    ta.root_dir = g_root;
    ta.cwd_dir = A20_HANDLE_NULL;
    ta.event_queue = A20_HANDLE_NULL;
    ta.argv = 0;
    ta.envp = 0;
    ta.argc = 0;
    ta.envc = 0;
    ta.handles = (uint64_t)(uintptr_t)&sh;
    ta.handle_count = 1;
    ta.flags = 0;
    ta.out_task = A20_HANDLE_NULL;
    ta.stdin_handle = A20_HANDLE_NULL;
    ta.stdout_handle = A20_HANDLE_NULL;
    ta.stderr_handle = A20_HANDLE_NULL;
    ta.reserved = 0;

    st = a20_syscall6(A20_SYS_task_spawn, (uint64_t)(uintptr_t)&ta, 0, 0, 0, 0, 0);
    a20_hdl_close(oa.out_handle);
    a20_hdl_close(pair.endpoints[1]);
    if (st < 0) {
        a20_hdl_close(pair.endpoints[0]);
        return st;
    }
    *out_ep = pair.endpoints[0];
    *out_task = ta.out_task;
    return A20_OK;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    g_root = si ? si->root_dir : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    objstats_t before;
    if (objstats_read(&before) != 0)
        return fail(1, "objects baseline read failed");

    for (int i = 0; i < AUDIT_CYCLES; i++) {
        a20_handle_t ep, task;
        if (spawn_echod(&ep, &task) != A20_OK)
            return fail(2, "spawn failed");
        /* Echo once so the service is definitely up, then crash it. */
        uint8_t req[4] = { 'p', 'i', 'n', 'g' };
        uint8_t rep[4];
        uint32_t rep_len = sizeof(rep);
        uint32_t rep_h = 0;
        if (a20_channel_call(ep, req, 4, 0, 0, rep, &rep_len, 0, &rep_h) < 0)
            return fail(3, "echo RPC failed");
        static const uint8_t crash_req[5] = { 'c', 'r', 'a', 's', 'h' };
        if (a20_channel_send(ep, crash_req, 5, 0, 0) != A20_OK)
            return fail(4, "crash request failed");

        a20_task_status_t ts;
        if (a20_task_wait(task, 0, &ts) != A20_OK)
            return fail(5, "task_wait failed");
        if (ts.exit_code != A20_SVC_CRASH_CODE)
            return fail(6, "unexpected exit code");
        a20_hdl_close(task);
        a20_hdl_close(ep);
    }

    objstats_t after;
    if (objstats_read(&after) != 0)
        return fail(7, "objects re-read failed");

    put_str("NATIVE_ISOLATION: before h=");
    put_u64(before.handles);
    put_str(" eps=");
    put_u64(before.channel_eps);
    put_str(" after h=");
    put_u64(after.handles);
    put_str(" eps=");
    put_u64(after.channel_eps);
    put("\n", 1);

    if (before.handles != after.handles ||
        before.channel_eps != after.channel_eps ||
        before.eventqs != after.eventqs ||
        before.vmos != after.vmos ||
        before.vmo_pages != after.vmo_pages ||
        before.irq_bindings != after.irq_bindings)
        return fail(8, "object counters drifted");

    put_str("NATIVE_ISOLATION: PASS cycles=");
    put_u64(AUDIT_CYCLES);
    put("\n", 1);
    return 0;
}

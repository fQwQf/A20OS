/*
 * svcmgr — service manager with a global registry (M3).
 *
 * Claims the well-known registry endpoint, spawns services from a static
 * manifest, serves register/lookup RPCs, watches services for EXITED and
 * respawns them (re-registering the fresh endpoint).  Runs forever as the
 * system supervisor; clients are arbitrary processes using the
 * service_registry handle from their start_info.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "../svc/rtcd_proto.h"

#define REG_OP_TAG   0x5245474FULL /* "REGO" — registry channel events */
#define SVC_EXIT_TAG 0x52545849ULL /* "RTXI" — service exit events */

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

/* ---- manifest ---- */

typedef struct {
    const char  *name;
    const char  *path;
    uint32_t     ep_slot;
    uint32_t     restarts;
    a20_handle_t task;
    a20_handle_t client_ep;   /* supervisor's client end handed to lookups */
} svc_entry_t;

#define SVC_MAX 4
static svc_entry_t g_svcs[SVC_MAX];
static uint32_t    g_nsvcs;

static a20_status_t spawn_one(svc_entry_t *se)
{
    a20_channel_pair_t pair;
    a20_status_t st = a20_channel_create(&pair);
    if (st != A20_OK)
        return st;

    a20_path_open_args_t oa;
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.flags = 0;
    oa.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    oa.path = (uint64_t)(uintptr_t)se->path;
    oa.path_len = 0;
    while (((const char *)(uintptr_t)oa.path)[oa.path_len]) oa.path_len++;
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
    sh.target_slot = se->ep_slot;
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
    ta.stdout_handle = g_out;
    ta.stderr_handle = g_out;
    ta.reserved = 0;

    st = a20_syscall6(A20_SYS_task_spawn, (uint64_t)(uintptr_t)&ta, 0, 0, 0, 0, 0);
    a20_hdl_close(oa.out_handle);
    a20_hdl_close(pair.endpoints[1]);
    if (st < 0) {
        a20_hdl_close(pair.endpoints[0]);
        return st;
    }
    if (se->client_ep != A20_HANDLE_NULL)
        a20_hdl_close(se->client_ep);
    se->client_ep = pair.endpoints[0];
    se->task = ta.out_task;
    return A20_OK;
}

/* ---- registry store: name -> client endpoint handle ---- */

static int reg_store_lookup(const char *name)
{
    for (uint32_t i = 0; i < g_nsvcs; i++) {
        uint32_t a = 0;
        while (a < 31 && g_svcs[i].name[a] && g_svcs[i].name[a] == name[a])
            a++;
        if (g_svcs[i].name[a] == name[a])
            return (int)i;
    }
    return -1;
}

static void registry_serve(a20_handle_t reg_ep)
{
    for (;;) {
        uint8_t buf[64];
        uint32_t blen = sizeof(buf);
        a20_handle_t hbuf[2];
        uint32_t hcnt = 2;
        a20_status_t st = a20_channel_recv_flags(reg_ep, buf, &blen, hbuf,
                                                 &hcnt, A20_MSG_NONBLOCK);
        if (st < 0)
            return; /* empty (WOULDBLOCK) or peer gone */
        if (blen < 4)
            continue;
        uint32_t op = (uint32_t)buf[0] | ((uint32_t)buf[1] << 8) |
                      ((uint32_t)buf[2] << 16) | ((uint32_t)buf[3] << 24);
        char name[32];
        uint32_t n = blen - 4;
        if (n > 31) n = 31;
        for (uint32_t i = 0; i < n; i++) name[i] = (char)buf[4 + i];
        name[n] = 0;

        if (op == A20_REG_OP_LOOKUP) {
            int idx = reg_store_lookup(name);
            int64_t status = idx >= 0 ? 0 : -1;
            a20_handle_t h = idx >= 0 ? g_svcs[idx].client_ep
                                      : A20_HANDLE_NULL;
            a20_channel_send(reg_ep, &status, sizeof(status),
                             h != A20_HANDLE_NULL ? &h : 0,
                             h != A20_HANDLE_NULL ? 1 : 0);
        }
        /* REGISTER is only used by svcmgr itself in this manifest-driven
         * setup; external registration can be added without protocol
         * changes. */
    }
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    g_root = si ? si->root_dir : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    a20_handle_t reg_ep = (a20_handle_t)a20_registry_claim();
    if ((int32_t)reg_ep < 0) {
        put_str("SVC_MGR: FAIL registry claim\n");
        return 1;
    }

    a20_handle_t eq;
    if (a20_event_queue_create(&eq) != A20_OK)
        return 2;
    if (a20_event_watch(eq, reg_ep,
                        A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                        REG_OP_TAG) != A20_OK)
        return 3;

    /* Manifest: rtcd (the user-space RTC driver from phase 4). */
    g_svcs[0].name = "rtcd";
    g_svcs[0].path = "/bin/rtcd-rv";
    g_svcs[0].ep_slot = A20_RTCD_EP_SLOT;
    g_svcs[0].restarts = 0;
    g_svcs[0].task = A20_HANDLE_NULL;
    g_svcs[0].client_ep = A20_HANDLE_NULL;
    g_nsvcs = 1;

    if (spawn_one(&g_svcs[0]) != A20_OK) {
        put_str("SVC_MGR: FAIL spawn rtcd\n");
        return 4;
    }
    if (a20_event_watch(eq, g_svcs[0].task,
                        A20_EVENT_MASK(A20_EVENT_EXITED),
                        SVC_EXIT_TAG) != A20_OK)
        return 5;

    put_str("SVC_MGR: ready services=");
    put_u64(g_nsvcs);
    put("\n", 1);

    for (;;) {
        a20_event_t ev;
        a20_time_t inf = { .secs = (uint64_t)-1, .nsecs = 0 };
        if (a20_event_wait(eq, inf, &ev) < 0)
            continue;

        if (ev.user_data == REG_OP_TAG) {
            registry_serve(reg_ep);
        } else if (ev.user_data == SVC_EXIT_TAG) {
            svc_entry_t *se = &g_svcs[0];
            put_str("SVC_MGR: crash detected exit_code=");
            put_u64(ev.data0);
            put_str(" restarts=");
            se->restarts++;
            put_u64(se->restarts);
            put("\n", 1);
            a20_event_cancel(eq, se->task);
            a20_hdl_close(se->task);

            a20_time_t bo = { .secs = 0, .nsecs = 50 * 1000 * 1000 };
            a20_thread_sleep(bo);
            if (spawn_one(se) != A20_OK) {
                put_str("SVC_MGR: FAIL respawn\n");
                return 6;
            }
            a20_event_watch(eq, se->task,
                            A20_EVENT_MASK(A20_EVENT_EXITED),
                            SVC_EXIT_TAG);
            put_str("SVC_MGR: healed\n");
        }
    }
}


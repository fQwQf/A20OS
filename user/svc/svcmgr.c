/*
 * svcmgr — hybrid-kernel service manager with a global registry,
 * declarative manifest, dependency-ordered startup, health probes and
 * flap protection (docs/hybrid-kernel/02-mainstream-plan.md M3/M2).
 *
 * Runs as the system supervisor: claims the registry endpoint, spawns
 * manifest services in dependency order, serves name lookups, watches
 * each service for EXITED, pings them periodically for liveness, and
 * restarts them with backoff bounded by a per-service restart budget.
 */
#include "liba20rt/a20_sdk.h"
#include "a20_services_idl.h"
#include "a20_string.h"
#include "liba20rt/crt0_a20.h"

#define REG_OP_TAG    0x5245474FULL /* "REGO" — registry channel events */
#define SVC_EXIT_BASE 0x52545800ULL /* exit event tag base: + index */

#define PING_PERIOD_MS    2000ULL
#define PING_DEADLINE_MS  1500ULL
#define RESTART_BACKOFF_MS 50ULL
#define RESTART_BUDGET     5u
#define RESTART_WINDOW_MS 30000ULL

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

static uint64_t now_ms(void)
{
    a20_time_ns_t t = 0;
    a20_clock_get(A20_CLOCK_MONOTONIC, &t);
    return t / 1000000ULL;
}

/* ---- 托管设备序号的板级配置（roadmap：清单不再固化 block_index）----
 * 块设备枚举随平台而异（QEMU virt / VF2 / LS2K1000）。与 net 的 a20.*
 * 键同一哲学：只认命令行/运行时配置，不做编译期板级表。svcmgr 从
 * /proc/cmdline 读 a20.ufsd_blk=<n>，缺省回落 QEMU 开发布局的 1。 */
static char     g_ufsd_args[32];
static unsigned g_ufsd_blk = 1;
static int      g_ufsd_blk_from_cmdline;

static void ufsd_blk_config_load(void)
{
    a20_path_open_args_t oa;
    a20_memset(&oa, 0, sizeof(oa));
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.path = (uint64_t)(uintptr_t)"/proc/cmdline";
    oa.path_len = (uint32_t)a20_strlen("/proc/cmdline");
    oa.rights = A20_RIGHT_READ;
    oa.flags = A20_PATH_OPEN_RDONLY;
    oa.mode = 0;
    oa.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_path_open(&oa)))
        return;

    char buf[512];
    uint64_t got = 0;
    if (a20_status_is_err(a20_hdl_read_buf(oa.out_handle, buf,
                                           sizeof(buf) - 1, &got))) {
        a20_hdl_close(oa.out_handle);
        return;
    }
    a20_hdl_close(oa.out_handle);
    buf[got < sizeof(buf) - 1 ? got : sizeof(buf) - 1] = '\0';

    static const char key[] = "a20.ufsd_blk=";
    size_t klen = sizeof(key) - 1;
    for (char *p = buf; *p;) {
        while (*p == ' ' || *p == '\t' || *p == '\n')
            p++;
        if (!*p)
            break;
        char *end = p;
        while (*end && *end != ' ' && *end != '\t' && *end != '\n')
            end++;
        if ((size_t)(end - p) > klen && a20_memcmp(p, key, klen) == 0) {
            unsigned v = 0;
            int digits = 0;
            for (char *q = p + klen; q < end && *q >= '0' && *q <= '9'; q++) {
                v = v * 10 + (unsigned)(*q - '0');
                digits++;
            }
            if (digits && v >= 1 && v <= 15) {
                g_ufsd_blk = v;
                g_ufsd_blk_from_cmdline = 1;
            }
        }
        p = end;
    }

    /* "/ufs <blk> fat" — mount point and fstype stay as manifest data. */
    unsigned i = 0;
    static const char prefix[] = "/ufs ";
    for (unsigned z = 0; z < sizeof(prefix) - 1 && i < sizeof(g_ufsd_args) - 1; z++)
        g_ufsd_args[i++] = prefix[z];
    char dec[8];
    unsigned n = 0, v = g_ufsd_blk;
    do { dec[n++] = (char)('0' + v % 10); v /= 10; } while (v && n < sizeof(dec));
    while (n && i < sizeof(g_ufsd_args) - 1)
        g_ufsd_args[i++] = dec[--n];
    static const char suffix[] = " fat";
    for (unsigned z = 0; z < sizeof(suffix) - 1 && i < sizeof(g_ufsd_args) - 1; z++)
        g_ufsd_args[i++] = suffix[z];
    g_ufsd_args[i] = '\0';
}

/* ---- manifest ---- */

typedef enum svc_state {
    SVC_BOOT, SVC_UP, SVC_PENDING_PING, SVC_DEAD, SVC_FAILED,
} svc_state_t;

typedef struct {
    const char  *name;
    const char  *path;
    const char  *args;           /* 空格分隔的 argv；NULL 表示无参数 */
    uint32_t     ep_slot;
    uint8_t      ping_kind;      /* 0 = SVCMGR_REQ_ECHO, 1 = RTCD_REQ_TIME */
    uint8_t      state;
    a20_handle_t task;
    a20_handle_t client_ep;
    a20_handle_t ping_ep;
    uint64_t     last_ok_ms;
    uint64_t     ping_sent_ms;
    uint32_t     restarts;
    uint32_t     window_restarts;
    uint64_t     window_start_ms;
} svc_entry_t;

#define SVC_MAX 4
static svc_entry_t g_svcs[SVC_MAX];
static uint32_t    g_nsvcs;

/* ---- spawn ---- */

static a20_status_t spawn_one(svc_entry_t *se)
{
    a20_channel_pair_t pair;
    a20_status_t st = a20_channel_create(&pair);
    if (st != A20_OK)
        return st;
    a20_channel_pair_t ping_pair;
    st = a20_channel_create(&ping_pair);
    if (st != A20_OK) {
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(pair.endpoints[1]);
        return st;
    }

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
        a20_hdl_close(ping_pair.endpoints[0]);
        a20_hdl_close(ping_pair.endpoints[1]);
        return st;
    }

    /* 服务端点 + 健康探测端点：两条独立通道，监管流量与客户端 RPC
     * 不共享任何队列（见 a20_services_idl.h A20_SVC_PING_SLOT 注释）。 */
    a20_spawn_handle_t sh[2];
    sh[0].handle = pair.endpoints[1];
    sh[0].rights = A20_RIGHT_READ | A20_RIGHT_WRITE;
    sh[0].target_slot = se->ep_slot;
    sh[0].flags = 0;
    sh[1].handle = ping_pair.endpoints[1];
    sh[1].rights = A20_RIGHT_READ | A20_RIGHT_WRITE;
    sh[1].target_slot = A20_SVC_PING_SLOT;
    sh[1].flags = 0;

    a20_task_spawn_args_t ta;
    ta.size = sizeof(ta);
    ta.version = 2;
    ta.image = oa.out_handle;
    ta.root_dir = g_root;
    ta.cwd_dir = A20_HANDLE_NULL;
    ta.event_queue = A20_HANDLE_NULL;
    ta.envp = 0;
    ta.envc = 0;

    /* argv：清单 args 字段按空格切分为指针数组（内核经 copy_arg_vector
     * 从本进程地址空间取字符串）。 */
    /* argv[0] 惯例为程序名；清单 args 提供其余参数 */
    static char args_buf[128];
    static char *args_vec[9];
    uint32_t nargs = 0;
    args_vec[nargs++] = (char *)se->path;
    if (se->args && se->args[0]) {
        a20_strncpy(args_buf, se->args, sizeof(args_buf) - 1);
        args_buf[sizeof(args_buf) - 1] = '\0';
        char *tok = args_buf;
        while (nargs < 9) {
            while (*tok == ' ')
                tok++;
            if (!*tok)
                break;
            args_vec[nargs++] = tok;
            char *sp = tok;
            while (*sp && *sp != ' ')
                sp++;
            if (!*sp)
                break;
            *sp++ = '\0';
            tok = sp;
        }
    }
    args_vec[nargs] = NULL; /* copy_arg_vector 以空指针结尾 */
    ta.argv = (uint64_t)(uintptr_t)args_vec;
    ta.argc = nargs;
    ta.handles = (uint64_t)(uintptr_t)sh;
    ta.handle_count = 2;
    ta.flags = 0;
    ta.out_task = A20_HANDLE_NULL;
    ta.stdin_handle = A20_HANDLE_NULL;
    ta.stdout_handle = g_out;
    ta.stderr_handle = g_out;
    ta.reserved = 0;

    st = a20_syscall6(A20_SYS_task_spawn, (uint64_t)(uintptr_t)&ta, 0, 0, 0, 0, 0);
    a20_hdl_close(oa.out_handle);
    a20_hdl_close(pair.endpoints[1]);
    a20_hdl_close(ping_pair.endpoints[1]);
    if (st < 0) {
        a20_hdl_close(pair.endpoints[0]);
        a20_hdl_close(ping_pair.endpoints[0]);
        return st;
    }
    if (se->client_ep != A20_HANDLE_NULL)
        a20_hdl_close(se->client_ep);
    if (se->ping_ep != A20_HANDLE_NULL)
        a20_hdl_close(se->ping_ep);
    se->client_ep = pair.endpoints[0];
    se->ping_ep = ping_pair.endpoints[0];
    se->task = ta.out_task;
    se->state = SVC_UP;
    se->last_ok_ms = now_ms();
    se->ping_sent_ms = 0;
    return A20_OK;
}

/* ---- restart with flap budget ---- */

static void svc_restart(a20_handle_t eq, svc_entry_t *se)
{
    uint64_t now = now_ms();
    if (se->window_start_ms == 0)
        se->window_start_ms = now;
    if (now - se->window_start_ms > RESTART_WINDOW_MS) {
        se->window_start_ms = now;
        se->window_restarts = 0;
    }
    se->window_restarts++;
    if (se->window_restarts > RESTART_BUDGET) {
        se->state = SVC_FAILED;
        put_str("SVC_MGR: FLAP budget exceeded, service marked failed name=");
        put_str(se->name);
        put("\n", 1);
        return;
    }

    a20_time_t bo = { .secs = 0, .nsecs = RESTART_BACKOFF_MS * 1000 * 1000 };
    a20_thread_sleep(bo);
    se->restarts++;
    a20_status_t rst = spawn_one(se);
    if (rst != A20_OK) {
        se->state = SVC_FAILED;
        put_str("SVC_MGR: respawn failed name=");
        put_str(se->name);
        put_str(" st=-");
        put_u64((uint64_t)-rst);
        put("\n", 1);
        return;
    }
    a20_event_watch(eq, se->task, A20_EVENT_MASK(A20_EVENT_EXITED),
                    SVC_EXIT_BASE + (uint64_t)(se - g_svcs));
    /* The new ping_ep is a fresh channel endpoint: the pong watch must
     * be re-registered on it or health pongs go to the dead endpoint. */
    a20_event_watch(eq, se->ping_ep,
                    A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                    (uint64_t)(100 + (se - g_svcs)));
    put_str("SVC_MGR: healed name=");
    put_str(se->name);
    put_str(" restarts=");
    put_u64(se->restarts);
    put("\n", 1);
}

/* ---- registry (name -> client endpoint) ---- */

static int reg_store_lookup(const char *name)
{
    for (uint32_t i = 0; i < g_nsvcs; i++) {
        uint32_t a = 0;
        while (a < 31 && g_svcs[i].name[a] && g_svcs[i].name[a] == name[a])
            a++;
        if (g_svcs[i].name[a] == name[a] && g_svcs[i].state == SVC_UP)
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
            return;
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
    }
}

/* ---- health ping ---- */

static void svc_ping(svc_entry_t *se)
{
    if (se->state == SVC_FAILED)
        return;
    uint64_t now = now_ms();

    if (se->state == SVC_PENDING_PING) {
        if (now - se->ping_sent_ms > PING_DEADLINE_MS) {
            put_str("SVC_MGR: health ping timeout name=");
            put_str(se->name);
            put("\n", 1);
            a20_task_kill(se->task, 0);
            /* EXITED event drives the restart path. */
        }
        return;
    }

    if (now - se->last_ok_ms > PING_PERIOD_MS) {
        /* Services parse versioned IDL envelopes (a20_services.idl), not the
         * legacy raw 'T'/'ping' bytes.  A bare envelope carrying the right
         * request type is a valid health probe: rtcd answers RTCD_REQ_TIME
         * with a time reply, echod echoes SVCMGR_REQ_ECHO back. */
        a20_idl_envelope_t env;
        env.version = A20_SERVICES_IDL_VERSION;
        env.size = sizeof(env);
        env.type = (se->ping_kind == 1) ? RTCD_REQ_TIME : SVCMGR_REQ_ECHO;
        if (a20_channel_send(se->ping_ep, &env, sizeof(env), 0, 0) == A20_OK) {
            se->state = SVC_PENDING_PING;
            se->ping_sent_ms = now;
        }
    }
}

static void svc_pong(svc_entry_t *se)
{
    uint8_t buf[64];
    uint32_t blen = sizeof(buf);
    uint32_t hcnt = 0;
    a20_status_t st = a20_channel_recv_flags(se->ping_ep, buf, &blen,
                                             0, &hcnt, A20_MSG_NONBLOCK);
    if (st >= 0) {
        se->state = SVC_UP;
        se->last_ok_ms = now_ms();
        se->ping_sent_ms = 0;
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

    /* Declarative manifest, spawn order = dependency order. */
    g_svcs[0].name = "rtcd";
    g_svcs[0].path = "/bin/rtcd-rv.a20drv";
    g_svcs[0].ep_slot = A20_RTCD_EP_SLOT;
    g_svcs[0].ping_kind = 1;
    g_svcs[1].name = "echod";
    g_svcs[1].path = "/bin/svc-echod-rv";
    g_svcs[1].ep_slot = 104u /* A20_SVC_ENDPOINT_SLOT */;
    g_svcs[1].ping_kind = 0;
    g_svcs[2].name = "ufsd";
    g_svcs[2].ep_slot = 104u;
    g_svcs[2].ping_kind = 0;
    g_svcs[2].path = "/bin/ufsd-rv";
    g_nsvcs = 3;

    /* 板级配置化的托管序号：命令行覆盖缺省，日志记录解析结果。 */
    ufsd_blk_config_load();
    g_svcs[2].args = g_ufsd_args;
    put_str("SVC_MGR: ufsd blk=");
    put_u64(g_ufsd_blk);
    put_str(g_ufsd_blk_from_cmdline ? " (cmdline)\n" : " (default)\n");

    for (uint32_t i = 0; i < g_nsvcs; i++) {
        g_svcs[i].task = A20_HANDLE_NULL;
        g_svcs[i].client_ep = A20_HANDLE_NULL;
        g_svcs[i].ping_ep = A20_HANDLE_NULL;
        g_svcs[i].state = SVC_BOOT;
        g_svcs[i].restarts = 0;
        g_svcs[i].window_restarts = 0;
        g_svcs[i].window_start_ms = 0;
        if (spawn_one(&g_svcs[i]) != A20_OK) {
            put_str("SVC_MGR: FAIL spawn name=");
            put_str(g_svcs[i].name);
            put("\n", 1);
            return 4;
        }
        if (a20_event_watch(eq, g_svcs[i].task,
                            A20_EVENT_MASK(A20_EVENT_EXITED),
                            SVC_EXIT_BASE + (uint64_t)i) != A20_OK)
            return 5;
        if (a20_event_watch(eq, g_svcs[i].ping_ep,
                            A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                            (uint64_t)(i + 100)) != A20_OK)
            return 6;
    }

    put_str("SVC_MGR: ready services=");
    put_u64(g_nsvcs);
    put("\n", 1);

    for (;;) {
        a20_event_t ev;
        a20_time_t to = { .secs = 1, .nsecs = 0 };
        a20_status_t st = a20_event_wait(eq, to, &ev);

        if (st >= 0 && ev.user_data == REG_OP_TAG) {
            registry_serve(reg_ep);
        } else if (st >= 0 && ev.user_data >= SVC_EXIT_BASE &&
                   ev.user_data < SVC_EXIT_BASE + SVC_MAX) {
            uint32_t i = (uint32_t)(ev.user_data - SVC_EXIT_BASE);
            svc_entry_t *se = &g_svcs[i];
            a20_event_cancel(eq, se->task);
            a20_hdl_close(se->task);
            if (ev.data0 == 0) {
                /* 干净退出（如按需服务发现目标设备不存在）：视为正常
                 * 完成，不消耗重启预算。 */
                se->state = SVC_DEAD;
                put_str("SVC_MGR: service completed name=");
                put_str(se->name);
                put("\n", 1);
            } else {
                se->state = SVC_DEAD;
                put_str("SVC_MGR: crash detected name=");
                put_str(se->name);
                put_str(" exit_code=");
                put_u64(ev.data0);
                put("\n", 1);
                svc_restart(eq, se);
            }
        } else if (st >= 0 && ev.user_data >= 100) {
            /* Pong (or a plain reply) from service (i+100). */
            svc_entry_t *se = &g_svcs[(uint32_t)(ev.user_data - 100)];
            svc_pong(se);
        }

        /* Health sweep: ping laggards, kill timed-out pending pings. */
        for (uint32_t i = 0; i < g_nsvcs; i++)
            svc_ping(&g_svcs[i]);
    }
}

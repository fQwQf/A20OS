/*
 * Native shared-VMO ring benchmark (docs/hybrid-kernel/01-roadmap.md ph.3).
 *
 * Transfers 16 MiB with an increasing byte pattern over two transports,
 * each with a separate consumer process verifying integrity:
 *   1. shared-VMO SPSC ring + futex doorbell (zero syscalls on the
 *      neither-full-nor-empty path)
 *   2. channel messages (16 KiB each, kernel-mediated copies)
 * Prints per-transport throughput and NATIVE_SHMRING: PASS when both
 * consumers report intact data.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"
#include "liba20rt/a20_shmring.h"
#include "../svc/shmring_proto.h"

#define CHAN_MSG_SIZE 16384

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
    put_str("NATIVE_SHMRING: FAIL ");
    put_str(msg);
    put("\n", 1);
    return code;
}

static uint64_t now_ns(void)
{
    a20_time_ns_t t = 0;
    a20_clock_get(A20_CLOCK_MONOTONIC, &t);
    return t;
}

static a20_status_t spawn_child(const char *path, a20_handle_t pass_h,
                                uint32_t pass_slot, a20_rights_t pass_rights,
                                a20_handle_t *out_task)
{
    a20_path_open_args_t oa;
    oa.size = sizeof(oa);
    oa.version = 1;
    oa.dir = A20_HANDLE_NULL;
    oa.flags = 0;
    oa.rights = A20_RIGHT_READ | A20_RIGHT_EXEC;
    oa.path = (uint64_t)(uintptr_t)path;
    oa.path_len = 0;
    while (((const char *)(uintptr_t)oa.path)[oa.path_len]) oa.path_len++;
    oa.mode = 0;
    oa.out_handle = A20_HANDLE_NULL;
    a20_status_t st = a20_path_open(&oa);
    if (st != A20_OK)
        return st;

    a20_spawn_handle_t sh;
    sh.handle = pass_h;
    sh.rights = pass_rights;
    sh.target_slot = pass_slot;
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
    if (st < 0)
        return st;
    *out_task = ta.out_task;
    return A20_OK;
}

static int wait_child_ok(a20_handle_t task)
{
    a20_task_status_t ts;
    if (a20_task_wait(task, 0, &ts) != A20_OK)
        return -1;
    return ts.exit_code;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    g_root = si ? si->root_dir : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;

    /* ---- 1. Shared-VMO ring transport ---- */
    a20_handle_t vmo = (a20_handle_t)a20_vm_create_object(A20_SHMRING_VMO_SIZE, 0);
    if ((int32_t)vmo < 0)
        return fail(1, "vm_create_object failed");
    uint64_t base = 0;
    if (a20_vm_map(vmo, A20_SHMRING_VMO_SIZE, 0,
                   A20_PROT_READ | A20_PROT_WRITE, &base) < 0)
        return fail(2, "vm_map failed");
    a20_shmring_t *r = (a20_shmring_t *)(uintptr_t)base;
    a20_shmring_init(r, A20_SHMRING_CAP);
    r->total_lo = A20_SHMRING_TOTAL;

    a20_handle_t ring_task;
    if (spawn_child("/bin/shmringd-rv", vmo, A20_SHMRING_VMO_SLOT,
                    A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_MAP,
                    &ring_task) != A20_OK)
        return fail(3, "shmringd spawn failed");
    a20_shmring_wait_ready(r);

    uint8_t wbuf[32768];
    uint64_t t0 = now_ns();
    uint32_t produced = 0;
    while (produced < A20_SHMRING_TOTAL) {
        uint32_t n = A20_SHMRING_TOTAL - produced;
        if (n > sizeof(wbuf)) n = sizeof(wbuf);
        for (uint32_t i = 0; i < n; i++)
            wbuf[i] = (uint8_t)((produced + i) & 0xff);
        a20_shmring_write(r, wbuf, n);
        produced += n;
    }
    a20_shmring_wait_done(r);
    uint64_t t1 = now_ns();
    int rc = wait_child_ok(ring_task);
    a20_hdl_close(ring_task);
    if (rc != 0)
        return fail(4, "ring data corrupted");

    /* ---- 2. Channel transport ---- */
    a20_channel_pair_t pair;
    if (a20_channel_create(&pair) != A20_OK)
        return fail(5, "channel_create failed");

    a20_handle_t chan_task;
    if (spawn_child("/bin/chand-rv", pair.endpoints[1], A20_CHAND_EP_SLOT,
                    A20_RIGHT_READ | A20_RIGHT_WRITE, &chan_task) != A20_OK)
        return fail(6, "chand spawn failed");
    a20_hdl_close(pair.endpoints[1]);

    uint8_t hdr[4] = {
        (uint8_t)(A20_SHMRING_TOTAL & 0xff),
        (uint8_t)((A20_SHMRING_TOTAL >> 8) & 0xff),
        (uint8_t)((A20_SHMRING_TOTAL >> 16) & 0xff),
        (uint8_t)((A20_SHMRING_TOTAL >> 24) & 0xff),
    };
    if (a20_channel_send(pair.endpoints[0], hdr, 4, 0, 0) != A20_OK)
        return fail(7, "channel header send failed");

    uint8_t cbuf[CHAN_MSG_SIZE];
    uint64_t t2 = now_ns();
    produced = 0;
    while (produced < A20_SHMRING_TOTAL) {
        uint32_t n = A20_SHMRING_TOTAL - produced;
        if (n > sizeof(cbuf)) n = sizeof(cbuf);
        for (uint32_t i = 0; i < n; i++)
            cbuf[i] = (uint8_t)((produced + i) & 0xff);
        if (a20_channel_send(pair.endpoints[0], cbuf, n, 0, 0) < 0)
            return fail(8, "channel send failed");
        produced += n;
    }
    rc = wait_child_ok(chan_task);
    uint64_t t3 = now_ns();
    a20_hdl_close(chan_task);
    a20_hdl_close(pair.endpoints[0]);
    if (rc != 0)
        return fail(9, "channel data corrupted");

    uint64_t ring_ns = t1 - t0;
    uint64_t chan_ns = t3 - t2;
    put_str("NATIVE_SHMRING: 16MiB ring_ns=");
    put_u64(ring_ns);
    put_str(" chan_ns=");
    put_u64(chan_ns);
    put_str(" ring_kib_s=");
    put_u64(ring_ns ? (A20_SHMRING_TOTAL / 1024) * 1000000000ull / ring_ns : 0);
    put_str(" chan_kib_s=");
    put_u64(chan_ns ? (A20_SHMRING_TOTAL / 1024) * 1000000000ull / chan_ns : 0);
    put("\n", 1);

    a20_hdl_close(vmo);
    put_str("NATIVE_SHMRING: PASS\n");
    return 0;
}

/*
 * Native ABI core-primitive contract tests
 * (docs/hybrid-kernel/03-refactor-plan.md phase 1).
 *
 * Each partition pins one externally observable contract:
 *   ralg — handle rights algebra (dup shrink-only, type mask, transfer ∩)
 *   bp   — channel backpressure/close semantics
 *   evqc — event queue semantics (timeout分级, duplicate-watch update, cancel)
 *   vmol — VMO lifecycle (lazy materialize, mapping outlives handle, reclaim)
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

static a20_handle_t g_stdout = A20_HANDLE_NULL;

static int fail(const char *what)
{
    if (g_stdout != A20_HANDLE_NULL) {
        char buf[128];
        int n = 0;
        buf[n++] = 'F';
        buf[n++] = ':';
        const char *p = what;
        while (*p && n < (int)sizeof(buf) - 2)
            buf[n++] = *p++;
        buf[n++] = '\n';
        a20_hdl_write_buf(g_stdout, buf, (uint64_t)n, NULL);
    }
    return 1;
}

static void note(const char *msg, uint64_t len)
{
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, msg, len, NULL);
}

static a20_handle_t open_file(const char *path, uint32_t flags, a20_rights_t rights)
{
    a20_path_open_args_t args;
    a20_memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.version = 1;
    args.dir = A20_HANDLE_NULL;
    args.path = (uint64_t)path;
    args.path_len = (uint32_t)a20_strlen(path);
    args.rights = rights;
    args.flags = flags;
    args.mode = 0644;
    args.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_path_open(&args)))
        return A20_HANDLE_NULL;
    return args.out_handle;
}

static a20_status_t chan_create_cap(a20_channel_pair_t *out, uint32_t cap)
{
    a20_channel_create_args_t args;
    a20_memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.version = 1;
    args.msg_capacity = cap;
    a20_status_t r = a20_syscall6(A20_SYS_channel_create, (uint64_t)&args, 0, 0, 0, 0, 0);
    if (a20_status_is_ok(r)) {
        out->endpoints[0] = args.out_endpoints[0];
        out->endpoints[1] = args.out_endpoints[1];
    }
    return r;
}

/* ---- ralg: handle rights algebra ---- */

static int rights_dup_superset_denied(void)
{
    const char *path = "/tmp/native_ralg_sup.txt";
    a20_handle_t h = open_file(path,
                               A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                               A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_DUP);
    if (h == A20_HANDLE_NULL)
        return fail("ralg-sup-open");

    a20_handle_dup_args_t d;
    a20_memset(&d, 0, sizeof(d));
    d.size = sizeof(d);
    d.version = 1;
    d.source = h;
    d.rights_mask = A20_RIGHT_READ | A20_RIGHT_SEEK; /* SEEK not in source */
    d.out_handle = A20_HANDLE_NULL;
    if (a20_hdl_dup(&d) != -A20_ERR_ACCESS)
        return fail("ralg-sup-access");

    a20_hdl_close(h);
    a20_path_unlink(A20_HANDLE_NULL, path, (uint32_t)a20_strlen(path));
    return 0;
}

static int rights_type_mask_converges(void)
{
    a20_channel_pair_t cp;
    if (a20_status_is_err(a20_channel_create(&cp)))
        return fail("ralg-mask-create");

    a20_handle_info_t info;
    a20_memset(&info, 0, sizeof(info));
    if (a20_status_is_err(a20_hdl_query(cp.endpoints[0], &info)))
        return fail("ralg-mask-query");
    /* docs/native-abi/03-handle.md: install converges to the type-valid mask;
     * channel endpoints carry exactly READ|WRITE|STAT|DUP|TRANSFER (STAT is
     * required for a20_hdl_query to observe the handle at all). */
    if (info.rights != (A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                        A20_RIGHT_DUP | A20_RIGHT_TRANSFER))
        return fail("ralg-mask-rights");

    a20_hdl_close(cp.endpoints[0]);
    a20_hdl_close(cp.endpoints[1]);
    return 0;
}

static a20_status_t chan_send_rights(a20_handle_t ep, const void *bytes, uint32_t n,
                                     const a20_handle_t *handles, uint32_t nh,
                                     const a20_rights_t *rights_arr)
{
    a20_msg_send_args_t args;
    a20_memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.version = 1;
    args.channel = ep;
    args.data = (uint64_t)bytes;
    args.data_len = n;
    args.handles = (uint64_t)handles;
    args.handle_count = nh;
    args.transfer_rights = (uint64_t)rights_arr;
    return a20_syscall6(A20_SYS_channel_send, (uint64_t)&args, 0, 0, 0, 0, 0);
}

static int rights_transfer_intersection(void)
{
    const char *path = "/tmp/native_ralg_xfer.txt";
    a20_handle_t f = open_file(path,
                               A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                               A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT |
                               A20_RIGHT_DUP | A20_RIGHT_TRANSFER);
    if (f == A20_HANDLE_NULL)
        return fail("ralg-xfer-open");

    /* Sender-side handle carries READ|STAT|TRANSFER. */
    a20_handle_dup_args_t d;
    a20_memset(&d, 0, sizeof(d));
    d.size = sizeof(d);
    d.version = 1;
    d.source = f;
    d.rights_mask = A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_TRANSFER;
    d.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_hdl_dup(&d)))
        return fail("ralg-xfer-dup");
    a20_handle_t h2 = d.out_handle;

    a20_channel_pair_t cp;
    if (a20_status_is_err(a20_channel_create(&cp)))
        return fail("ralg-xfer-chan");

    /* ρ_recv = ρ_send ∩ ρ_transfer = {R,S,T} ∩ {R,W,S} = {R,S}. */
    a20_rights_t tr[1] = { A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_STAT };
    const char tag[4] = { 'r', 'a', 'l', 'g' };
    if (a20_status_is_err(chan_send_rights(cp.endpoints[0], tag, 4, &h2, 1, tr)))
        return fail("ralg-xfer-send");

    char buf[8];
    uint32_t nb = sizeof(buf);
    a20_handle_t rh = A20_HANDLE_NULL;
    uint32_t nh = 1;
    if (a20_status_is_err(a20_channel_recv(cp.endpoints[1], buf, &nb, &rh, &nh)))
        return fail("ralg-xfer-recv");
    if (nh != 1 || rh == A20_HANDLE_NULL)
        return fail("ralg-xfer-count");

    a20_handle_info_t info;
    a20_memset(&info, 0, sizeof(info));
    if (a20_status_is_err(a20_hdl_query(rh, &info)))
        return fail("ralg-xfer-query");
    if (info.rights != (A20_RIGHT_READ | A20_RIGHT_STAT))
        return fail("ralg-xfer-intersect");

    a20_iovec_t wiov = { (uint64_t)"x", 1 };
    if (a20_status_is_ok(a20_hdl_write(rh, &wiov, 1, NULL)))
        return fail("ralg-xfer-wdenied");

    /* Intersection of zero is rejected at send time. */
    a20_rights_t tr0[1] = { A20_RIGHT_EXEC };
    if (chan_send_rights(cp.endpoints[0], tag, 4, &h2, 1, tr0) != -A20_ERR_ACCESS)
        return fail("ralg-xfer-zero");

    a20_hdl_close(rh);
    a20_hdl_close(h2);
    a20_hdl_close(f);
    a20_hdl_close(cp.endpoints[0]);
    a20_hdl_close(cp.endpoints[1]);
    a20_path_unlink(A20_HANDLE_NULL, path, (uint32_t)a20_strlen(path));
    return 0;
}

static int rights_algebra(void)
{
    if (rights_dup_superset_denied() != 0)
        return 1;
    if (rights_type_mask_converges() != 0)
        return 1;
    if (rights_transfer_intersection() != 0)
        return 1;
    return 0;
}

/* ---- bp: channel backpressure ---- */

static int channel_backpressure(void)
{
    a20_channel_pair_t cp;
    if (a20_status_is_err(chan_create_cap(&cp, 2)))
        return fail("bp-create");
    a20_handle_t tx = cp.endpoints[0];
    a20_handle_t rx = cp.endpoints[1];

    const char m0[2] = { 'm', '0' };
    const char m1[2] = { 'm', '1' };
    const char m2[2] = { 'm', '2' };
    if (a20_status_is_err(a20_channel_send_flags(tx, m0, 2, NULL, 0, A20_MSG_NONBLOCK)))
        return fail("bp-send0");
    if (a20_status_is_err(a20_channel_send_flags(tx, m1, 2, NULL, 0, A20_MSG_NONBLOCK)))
        return fail("bp-send1");
    /* Full queue: NONBLOCK fails with WOULD_BLOCK, never sleeps. */
    if (a20_channel_send_flags(tx, m2, 2, NULL, 0, A20_MSG_NONBLOCK) !=
        -A20_ERR_WOULD_BLOCK)
        return fail("bp-full");

    /* FIFO: draining one slot unblocks the next send. */
    char buf[4];
    uint32_t nb = sizeof(buf);
    uint32_t nh = 0;
    if (a20_status_is_err(a20_channel_recv(rx, buf, &nb, NULL, &nh)))
        return fail("bp-recv0");
    if (nb != 2 || buf[0] != 'm' || buf[1] != '0')
        return fail("bp-fifo0");
    if (a20_status_is_err(a20_channel_send_flags(tx, m2, 2, NULL, 0, A20_MSG_NONBLOCK)))
        return fail("bp-send2");

    nb = sizeof(buf);
    if (a20_status_is_err(a20_channel_recv(rx, buf, &nb, NULL, &nh)))
        return fail("bp-recv1");
    if (nb != 2 || buf[1] != '1')
        return fail("bp-fifo1");
    nb = sizeof(buf);
    if (a20_status_is_err(a20_channel_recv(rx, buf, &nb, NULL, &nh)))
        return fail("bp-recv2");
    if (nb != 2 || buf[1] != '2')
        return fail("bp-fifo2");

    if (a20_channel_recv_flags(rx, buf, &nb, NULL, &nh, A20_MSG_NONBLOCK) !=
        -A20_ERR_WOULD_BLOCK)
        return fail("bp-empty");

    /* Peer close: send and drained-recv both report CANCELED. */
    if (a20_status_is_err(a20_hdl_close(tx)))
        return fail("bp-close");
    if (a20_channel_send_flags(rx, m0, 2, NULL, 0, A20_MSG_NONBLOCK) !=
        -A20_ERR_CANCELED)
        return fail("bp-send-canceled");
    nb = sizeof(buf);
    if (a20_channel_recv_flags(rx, buf, &nb, NULL, &nh, A20_MSG_NONBLOCK) !=
        -A20_ERR_CANCELED)
        return fail("bp-recv-canceled");

    a20_hdl_close(rx);
    return 0;
}

/* ---- evqc: event queue semantics ---- */

static int eventq_semantics(void)
{
    a20_channel_pair_t cp;
    if (a20_status_is_err(a20_channel_create(&cp)))
        return fail("evqc-create");
    a20_handle_t tx = cp.endpoints[0];
    a20_handle_t rx = cp.endpoints[1];

    a20_handle_t q;
    if (a20_status_is_err(a20_event_queue_create(&q)))
        return fail("evqc-queue");
    /* Contract: channel message arrival raises MESSAGE_READY (not READABLE);
     * peer close raises PEER_CLOSED (kernel/ipc/a20_channel.c). */
    if (a20_status_is_err(a20_event_watch(q, rx, A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                                          0xAAAA)))
        return fail("evqc-watch");

    /* timeout = 0 is a poll: empty queue reports WOULD_BLOCK. */
    a20_event_t ev;
    a20_time_t t0 = { .secs = 0, .nsecs = 0 };
    if (a20_event_wait(q, t0, &ev) != -A20_ERR_WOULD_BLOCK)
        return fail("evqc-poll");

    /* Delivery: source handle, event mask and user_data round-trip. */
    const char m[1] = { 'e' };
    if (a20_status_is_err(a20_channel_send(tx, m, 1, NULL, 0)))
        return fail("evqc-send0");
    a20_time_t t1 = { .secs = 1, .nsecs = 0 };
    a20_status_t r = a20_event_wait(q, t1, &ev);
    if (r != 1)
        return fail("evqc-wait0");
    if (ev.source != rx || !(ev.events & A20_EVENT_MASK(A20_EVENT_MESSAGE_READY)) ||
        ev.user_data != 0xAAAA)
        return fail("evqc-event0");

    char buf[8];
    uint32_t nb = sizeof(buf);
    uint32_t nh = 0;
    if (a20_status_is_err(a20_channel_recv(rx, buf, &nb, NULL, &nh)))
        return fail("evqc-drain0");

    /* Re-watch of the same object+mask updates user_data in place. */
    if (a20_status_is_err(a20_event_watch(q, rx, A20_EVENT_MASK(A20_EVENT_MESSAGE_READY),
                                          0xBBBB)))
        return fail("evqc-rewatch");
    if (a20_status_is_err(a20_channel_send(tx, m, 1, NULL, 0)))
        return fail("evqc-send1");
    r = a20_event_wait(q, t1, &ev);
    if (r != 1)
        return fail("evqc-wait1");
    if (ev.user_data != 0xBBBB)
        return fail("evqc-update");

    nb = sizeof(buf);
    if (a20_status_is_err(a20_channel_recv(rx, buf, &nb, NULL, &nh)))
        return fail("evqc-drain1");

    /* Cancel stops delivery; a finite wait reports TIMED_OUT. */
    if (a20_status_is_err(a20_event_cancel(q, rx)))
        return fail("evqc-cancel");
    if (a20_status_is_err(a20_channel_send(tx, m, 1, NULL, 0)))
        return fail("evqc-send2");
    a20_time_t t50 = { .secs = 0, .nsecs = 50 * 1000 * 1000 };
    if (a20_event_wait(q, t50, &ev) != -A20_ERR_TIMED_OUT)
        return fail("evqc-timeout");

    nb = sizeof(buf);
    a20_channel_recv(rx, buf, &nb, NULL, &nh);

    /* Peer close raises PEER_CLOSED on the surviving endpoint. */
    if (a20_status_is_err(a20_event_watch(q, rx, A20_EVENT_MASK(A20_EVENT_PEER_CLOSED),
                                          0xCCCC)))
        return fail("evqc-watch-close");
    if (a20_status_is_err(a20_hdl_close(tx)))
        return fail("evqc-close-tx");
    r = a20_event_wait(q, t1, &ev);
    if (r != 1)
        return fail("evqc-wait-close");
    if (!(ev.events & A20_EVENT_MASK(A20_EVENT_PEER_CLOSED)) || ev.user_data != 0xCCCC)
        return fail("evqc-peer-closed");
    tx = A20_HANDLE_NULL;

    a20_hdl_close(q);
    a20_hdl_close(rx);
    return 0;
}

/* ---- vmol: VMO lifecycle ---- */

static int read_objstat(const char *key, uint64_t *out)
{
    a20_handle_t f = open_file("/proc/a20/objects", A20_PATH_OPEN_RDONLY,
                               A20_RIGHT_READ);
    if (f == A20_HANDLE_NULL)
        return -1;
    char buf[256];
    uint64_t got = 0;
    a20_status_t r = a20_hdl_read_buf(f, buf, sizeof(buf) - 1, &got);
    a20_hdl_close(f);
    if (a20_status_is_err(r) || got == 0)
        return -1;
    buf[got] = '\0';

    uint32_t klen = a20_strlen(key);
    for (char *p = buf; *p; ) {
        char *nl = p;
        while (*nl && *nl != '\n')
            nl++;
        if ((uint64_t)(nl - p) > klen + 2 && a20_memcmp(p, key, klen) == 0 &&
            p[klen] == ':' && p[klen + 1] == ' ') {
            uint64_t v = 0;
            for (char *d = p + klen + 2; d < nl; d++) {
                if (*d < '0' || *d > '9')
                    return -1;
                v = v * 10 + (uint64_t)(*d - '0');
            }
            *out = v;
            return 0;
        }
        p = *nl ? nl + 1 : nl;
    }
    return -1;
}

static int vmo_lifecycle(void)
{
    uint64_t vmos0 = 0, pages0 = 0;
    if (read_objstat("vmos", &vmos0) != 0 || read_objstat("vmo_pages", &pages0) != 0)
        return fail("vmol-baseline");

    a20_status_t r = a20_vm_create_object(3 * 4096, 0);
    if (r < 0)
        return fail("vmol-create");
    a20_handle_t vmo = (a20_handle_t)r;

    uint64_t addr = 0;
    if (a20_status_is_err(a20_vm_map(vmo, 3 * 4096, 0,
                                     A20_PROT_READ | A20_PROT_WRITE, &addr)))
        return fail("vmol-map");

    uint64_t v = 0, p = 0;
    if (read_objstat("vmos", &v) != 0 || v != vmos0 + 1)
        return fail("vmol-count-create");

    /* Fresh VMO pages must read as zero (even before any write). */
    volatile uint8_t *base = (volatile uint8_t *)addr;
    if (base[0] != 0 || base[2 * 4096] != 0)
        return fail("vmol-zero");

    /* Lazy materialization: the two touches above fault exactly two pages. */
    if (read_objstat("vmo_pages", &p) != 0 || p != pages0 + 2)
        return fail("vmol-count-lazy");

    base[0] = 0x5A;
    base[2 * 4096] = 0xA5;

    /* The mapping pins the VMO: closing the handle must not invalidate it. */
    if (a20_status_is_err(a20_hdl_close(vmo)))
        return fail("vmol-close");
    if (base[0] != 0x5A || base[2 * 4096] != 0xA5)
        return fail("vmol-content");
    if (base[4096] != 0) /* a fresh page fault still resolves through the VMA */
        return fail("vmol-zero-after-close");
    base[4096] = 0x11;
    if (read_objstat("vmo_pages", &p) != 0 || p != pages0 + 3)
        return fail("vmol-count-after-close");
    if (read_objstat("vmos", &v) != 0 || v != vmos0 + 1)
        return fail("vmol-count-pinned");

    if (a20_status_is_err(a20_vm_unmap(addr, 3 * 4096)))
        return fail("vmol-unmap");
    if (read_objstat("vmos", &v) != 0 || v != vmos0)
        return fail("vmol-leak-vmo");
    if (read_objstat("vmo_pages", &p) != 0 || p != pages0)
        return fail("vmol-leak-pages");
    return 0;
}

static int dma_heap_contiguous(void)
{
    a20_status_t create = a20_device_alloc_dma(4 * 4096);
    if (create < 0)
        return fail("dma-create");
    a20_handle_t dma = (a20_handle_t)create;

    uint64_t addr = 0;
    if (a20_status_is_err(a20_vm_map(dma, 4 * 4096, 0,
                                     A20_PROT_READ | A20_PROT_WRITE, &addr)))
        return fail("dma-map");

    uint64_t phys[4] = {0};
    uint32_t count = 0;
    if (a20_device_vmo_phys(dma, phys, 4, &count) != A20_OK || count != 4)
        return fail("dma-phys");
    for (uint32_t i = 1; i < count; i++)
        if (phys[i] != phys[0] + (uint64_t)i * 4096)
            return fail("dma-not-contiguous");

    volatile uint8_t *p = (volatile uint8_t *)(uintptr_t)addr;
    if (p[0] != 0 || p[4095] != 0 || p[4096] != 0 || p[3 * 4096] != 0)
        return fail("dma-not-zero");
    p[0] = 0xA5;
    p[3 * 4096] = 0x5A;
    if (a20_vm_unmap(addr, 4 * 4096) != A20_OK)
        return fail("dma-unmap");
    if (a20_hdl_close(dma) != A20_OK)
        return fail("dma-close");
    return 0;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;

    a20_start_info_t *si = a20_get_start_info();
    if (si)
        g_stdout = si->stdout_handle;

    note("start\n", 6);

    if (rights_algebra() != 0)
        return 1;
    note("ralg ok\n", 8);

    if (channel_backpressure() != 0)
        return 1;
    note("bp ok\n", 6);

    if (eventq_semantics() != 0)
        return 1;
    note("evqc ok\n", 8);

    if (vmo_lifecycle() != 0)
        return 1;
    note("vmol ok\n", 8);

    if (dma_heap_contiguous() != 0)
        return 1;
    note("dma ok\n", 7);

    note("NATIVE_CONTRACT: PASS\n", 20);
    return 0;
}

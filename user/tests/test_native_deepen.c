/*
 * Native ABI deepening smoke test.
 *
 * Exercises the mechanisms added in docs/native-abi/09-native-abi-deepening.md:
 *   1. Pager: a PAGED VMO whose page faults are satisfied by a user-space
 *      pager thread over the request channel.
 *   2. Monitor: a perf-style counter object sampled on demand.
 *   3. task_mem: rights-checked cross-task memory access (thread target).
 *   4. vm_share_region: export an address range as a new MEMORY handle.
 *   5. vm_protect capability: extending beyond the creation-time cap fails.
 *   6. EventQ FS events: event_watch_fs on a directory fires CREATE.
 *   7. EventQ socket source: socketpair write fires READABLE on the peer.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/crt0_a20.h"

static a20_handle_t g_out = A20_HANDLE_NULL;

static volatile uint32_t g_pager_requests_seen;
static volatile uint32_t g_pager_supplied;
static volatile uint32_t g_pager_fail;

static a20_handle_t g_requests = A20_HANDLE_NULL;
static a20_handle_t g_pager_h = A20_HANDLE_NULL;
static a20_handle_t g_source_vmo = A20_HANDLE_NULL;

#define MAGIC_BYTE 0x5A

/* Pager thread: recv a page request (16-byte payload + VMO handle), then
 * supply the page from the source VMO. */
static void pager_worker(uint64_t arg)
{
    (void)arg;
    for (;;) {
        uint8_t msg[16];
        a20_handle_t handles[4];
        uint32_t nb = 16;
        uint32_t nh = 4;
        a20_status_t r = a20_channel_recv(g_requests, msg, &nb, handles, &nh);
        if (r < 0 || nh < 1) {
            g_pager_fail = (uint32_t)(-r);
            a20_thread_exit(1);
        }
        g_pager_requests_seen++;
        uint64_t offset = 0;
        if (nb >= 16) {
            offset = ((uint64_t)msg[8]) | ((uint64_t)msg[9] << 8) |
                     ((uint64_t)msg[10] << 16) | ((uint64_t)msg[11] << 24) |
                     ((uint64_t)msg[12] << 32) | ((uint64_t)msg[13] << 40) |
                     ((uint64_t)msg[14] << 48) | ((uint64_t)msg[15] << 56);
        }
        uint64_t supplied = 0;
        a20_status_t sr = a20_pager_supply_pages(g_pager_h, handles[0],
                                                 g_source_vmo, offset, 0,
                                                 4096, &supplied);
        a20_hdl_close(handles[0]);
        if (sr < 0 || supplied != 4096) {
            g_pager_fail = (uint32_t)(-sr);
            g_pager_supplied = 1;
            a20_thread_exit(2);
        }
        g_pager_supplied++;
        /* Keep serving: a pager thread lives for the process lifetime. */
    }
}

static void a20_itoa(int64_t v, char *out)
{
    int64_t n = v < 0 ? -v : v;
    char tmp[20];
    int i = 0;
    do { tmp[i++] = (char)('0' + (n % 10)); n /= 10; } while (n);
    int j = 0;
    if (v < 0) out[j++] = '-';
    while (i > 0) out[j++] = tmp[--i];
    out[j] = 0;
}

static int fail(int code, const char *msg, uint32_t len)
{
    if (g_out != A20_HANDLE_NULL) {
        a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: FAIL ", 20, (void *)0);
        a20_hdl_write_buf(g_out, msg, len, (void *)0);
        a20_hdl_write_buf(g_out, " code=", 6, (void *)0);
        char tmp[16];
        a20_itoa(code, tmp);
        a20_hdl_write_buf(g_out, tmp, (uint32_t)a20_strlen(tmp), (void *)0);
        a20_hdl_write_buf(g_out, "\n", 1, (void *)0);
    }
    return code;
}

static a20_status_t spawn_pager_thread(void (*fn)(uint64_t))
{
    uint64_t stack;
    a20_status_t st = a20_vm_alloc_pages(16, 3 /* RW */, &stack);
    if (st < 0) return st;
    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)fn;
    tc.arg = 0;
    tc.stack_base = (stack + 16 * 4096) & ~15ULL;
    tc.stack_size = 16 * 4096;
    tc.tls_base = 0;
    return a20_thread_create(&tc);
}

int main(int argc, char **argv, char **envp)
{
    (void)argc; (void)argv; (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    g_out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (g_out == A20_HANDLE_NULL)
        return 90;
    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: START\n", 21, (void *)0);
    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: step-pager\n", 26, (void *)0);

    /* ---- 1. Pager ---- */
    /* Source VMO: one page of MAGIC_BYTE. */
    a20_status_t st = a20_vm_create_object(4096, 0);
    if (st < 0) return fail(1, "source vmo create", 18);
    g_source_vmo = (a20_handle_t)st;
    uint64_t src_addr = 0;
    st = a20_vm_map(g_source_vmo, 4096, 0, 3 /* RW */, &src_addr);
    if (st < 0 || src_addr == 0) return fail(2, "source vmo map", 15);
    for (uint64_t i = 0; i < 4096; i++)
        ((volatile uint8_t *)src_addr)[i] = MAGIC_BYTE;

    /* Pager object + request channel. */
    st = a20_pager_create(0, &g_pager_h, &g_requests);
    if (st < 0) return fail(3, "pager create", 13);

    /* PAGED VMO, attach, map. */
    st = a20_vm_create_object(4096, A20_VMO_PAGED);
    if (st < 0) return fail(4, "paged vmo create", 17);
    a20_handle_t paged_vmo = (a20_handle_t)st;
    st = a20_pager_vmo_attach(g_pager_h, paged_vmo);
    if (st < 0) return fail(5, "pager attach", 13);
    uint64_t paged_addr = 0;
    st = a20_vm_map(paged_vmo, 4096, 0, 1 /* R */, &paged_addr);
    if (st < 0 || paged_addr == 0) return fail(6, "paged vmo map", 14);

    /* Start the pager thread, then fault page 0. */
    st = spawn_pager_thread(pager_worker);
    if (st < 0) return fail(7, "pager thread", 13);
    a20_handle_t pager_thread = (a20_handle_t)st;
    uint8_t got = ((volatile uint8_t *)paged_addr)[0];
    if (g_pager_fail) return fail(8, "pager thread error", 19);
    /* The read blocks until the pager supplies the page; the byte check below
     * is the proof of a successful request+supply round trip. */
    if (got != MAGIC_BYTE) return fail(10, "pager data mismatch", 19);

    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: step-monitor\n", 26, (void *)0);
    /* ---- 2. Monitor ---- */
    a20_handle_t mon = A20_HANDLE_NULL;
    st = a20_monitor_create(A20_HANDLE_NULL, A20_MONITOR_SYS_PAGE_FAULTS,
                            0, &mon);
    if (st < 0) return fail(11, "monitor create", 15);
    a20_monitor_value_t v0, v1;
    st = a20_monitor_query(mon, &v0);
    if (st < 0) return fail(12, "monitor query1", 15);
    /* Cause a page fault so a system counter can advance. */
    volatile uint64_t probe = 0x1234;
    probe += 1;
    st = a20_monitor_query(mon, &v1);
    if (st < 0) return fail(13, "monitor query2", 15);
    if (v1.count < v0.count)
        return fail(14, "monitor not monotonic", 21);

    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: step-taskmem\n", 27, (void *)0);
    /* ---- 3. task_mem (thread shares the address space) ---- */
    volatile uint32_t shared = 0;
    shared = 0xCAFE;
    uint64_t xfer = 0;
    a20_iovec_t lv = { .base = (uint64_t)&shared, .len = sizeof(shared) };
    a20_iovec_t rv = { .base = (uint64_t)&shared, .len = sizeof(shared) };
    st = a20_task_mem(pager_thread, 1, &lv, 1, &rv, 1, &xfer);
    if (st < 0 || xfer != sizeof(shared)) {
        a20_hdl_write_buf(g_out, "task_mem st=", 12, (void *)0);
        char tmp[16];
        a20_itoa((int64_t)st, tmp);
        a20_hdl_write_buf(g_out, tmp, (uint32_t)a20_strlen(tmp), (void *)0);
        a20_hdl_write_buf(g_out, "\n", 1, (void *)0);
        return fail(15, "task_mem read", 13);
    }

    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: step-share\n", 25, (void *)0);
    /* ---- 4. vm_share_region ---- */
    a20_handle_t shared_h = A20_HANDLE_NULL;
    st = a20_vm_share_region(src_addr, 4096, A20_RIGHT_READ | A20_RIGHT_MAP,
                             &shared_h);
    if (st < 0) return fail(16, "vm_share_region", 15);
    uint64_t map2 = 0;
    st = a20_vm_map(shared_h, 4096, 0, 1 /* R */, &map2);
    if (st < 0 || map2 == 0) return fail(17, "shared map", 10);
    if (((volatile uint8_t *)map2)[0] != MAGIC_BYTE)
        return fail(18, "shared data", 11);

    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: step-cap\n", 23, (void *)0);
    /* ---- 5. vm_protect capability ---- */
    uint64_t cap_addr = 0;
    st = a20_vm_alloc_pages(2, 3 /* RW */, &cap_addr);
    if (st < 0 || cap_addr == 0) return fail(19, "cap alloc", 9);
    /* RW -> RWX must be denied (creation cap was R|W only). */
    st = a20_syscall6(A20_SYS_vm_protect, cap_addr, 8192,
                      1 /* R */ | 2 /* W */ | 4 /* X */, 0, 0, 0);
    if (st == A20_OK) return fail(20, "vm_protect over cap allowed", 28);

    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: step-fswatch\n", 26, (void *)0);
    /* ---- 6. FS watch ---- */
    a20_handle_t queue = A20_HANDLE_NULL;
    st = a20_event_queue_create(&queue);
    if (st < 0) return fail(21, "fs evq", 6);
    uint64_t mkdir_path = 0;
    st = a20_vm_alloc_pages(4, 3, &mkdir_path);
    if (st < 0) return fail(22, "fs path buf", 12);
    a20_strcpy((char *)mkdir_path, "/tmp");
    a20_handle_t dir = A20_HANDLE_NULL;
    {
        a20_path_open_args_t args = {0};
        args.size = sizeof(args);
        args.version = 1;
        args.dir = A20_HANDLE_NULL;
        args.flags = 0x40; /* O_DIRECTORY */
        args.rights = A20_RIGHT_READ | A20_RIGHT_CONTROL | A20_RIGHT_STAT;
        args.path = mkdir_path;
        args.path_len = 4;
        a20_syscall6(A20_SYS_path_open, (uint64_t)&args, 0, 0, 0, 0, 0);
        dir = args.out_handle;
    }
    if (dir == A20_HANDLE_NULL) return fail(23, "fs dir open", 12);
    st = a20_event_watch_fs(queue, dir, 0, 0,
                            A20_EVENT_MASK(A20_EVENT_FS_CREATE), 0xfeed);
    if (st < 0) return fail(24, "fs watch", 9);

    /* Create a file inside the watched directory.  The image persists between
     * smoke runs, so remove any leftover file first to guarantee O_CREAT. */
    uint64_t create_path = mkdir_path + 4096;
    a20_strcpy((char *)create_path, "/tmp/watchfile");
    {
        a20_path_unlink_args_t ul = {0};
        (void)ul;
        a20_path_unlink(A20_HANDLE_NULL, (const char *)create_path, 14);
    }
    {
        a20_path_open_args_t args = {0};
        args.size = sizeof(args);
        args.version = 1;
        args.dir = A20_HANDLE_NULL;
        args.flags = 0x41; /* O_CREAT */
        args.rights = A20_RIGHT_READ | A20_RIGHT_WRITE;
        args.path = create_path;
        args.path_len = 14;
        args.mode = 0644;
        a20_status_t cr = a20_syscall6(A20_SYS_path_open, (uint64_t)&args,
                                       0, 0, 0, 0, 0);
        if (cr < 0)
            return fail(33, "fs create", 9);
        if (args.out_handle != A20_HANDLE_NULL)
            a20_hdl_close(args.out_handle);
    }

    a20_event_t ev;
    a20_memset(&ev, 0, sizeof(ev));
    a20_time_t to = { .secs = 2, .nsecs = 0 };
    st = a20_event_wait(queue, to, &ev);
    if (st < 0) return fail(25, "fs event wait", 14);
    if (ev.type != A20_EVENT_FS_CREATE || ev.user_data != 0xfeed)
        return fail(26, "fs event payload", 16);

    a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: step-sockev\n", 25, (void *)0);
    /* ---- 7. Socket event source ---- */
    a20_handle_t sp_out[2];
    st = a20_net_socketpair(1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, sp_out);
    if (st < 0) return fail(27, "socketpair", 11);

    a20_handle_t sockq = A20_HANDLE_NULL;
    st = a20_event_queue_create(&sockq);
    if (st < 0) return fail(28, "sock evq", 8);
    st = a20_event_watch(sockq, sp_out[0],
                         A20_EVENT_MASK(A20_EVENT_READABLE), 0x1234);
    if (st < 0) return fail(29, "sock watch", 11);

    /* Write to the peer; the read end should become readable. */
    const char hello[] = "ping";
    st = a20_hdl_write_buf(sp_out[1], hello, 4, (void *)0);
    if (st < 0) return fail(30, "sock write", 11);

    a20_memset(&ev, 0, sizeof(ev));
    st = a20_event_wait(sockq, to, &ev);
    if (st < 0) return fail(31, "sock event wait", 16);
    if (ev.type != A20_EVENT_READABLE || ev.user_data != 0x1234)
        return fail(32, "sock event payload", 18);

    if (g_out != A20_HANDLE_NULL) {
        a20_hdl_write_buf(g_out, "NATIVE_DEEPEN: PASS\n", 21, (void *)0);
    }
    return 0;
}

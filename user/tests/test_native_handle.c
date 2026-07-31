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

static int handle_dup_rights_downgrade(void)
{
    const char *path = "/tmp/native_dup_src.txt";
    a20_handle_t h = open_file(path,
                               A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                               A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_DUP |
                               A20_RIGHT_STAT);
    if (h == A20_HANDLE_NULL)
        return fail("dup-open");

    a20_handle_info_t info;
    a20_memset(&info, 0, sizeof(info));
    if (a20_status_is_err(a20_hdl_query(h, &info)))
        return fail("dup-query-src");
    if (info.rights != (A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_DUP | A20_RIGHT_STAT))
        return fail("dup-src-rights");

    a20_handle_dup_args_t dup_args;
    a20_memset(&dup_args, 0, sizeof(dup_args));
    dup_args.size = sizeof(dup_args);
    dup_args.version = 1;
    dup_args.source = h;
    dup_args.rights_mask = A20_RIGHT_READ | A20_RIGHT_STAT;
    dup_args.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_hdl_dup(&dup_args)))
        return fail("dup");
    a20_handle_t h2 = dup_args.out_handle;

    a20_memset(&info, 0, sizeof(info));
    if (a20_status_is_err(a20_hdl_query(h2, &info)))
        return fail("dup-query-dst");
    if (info.rights != (A20_RIGHT_READ | A20_RIGHT_STAT))
        return fail("dup-dst-rights");

    a20_iovec_t iov = { (uint64_t)"x", 1 };
    if (a20_status_is_ok(a20_hdl_write(h2, &iov, 1, NULL)))
        return fail("dup-write-denied");

    a20_hdl_close(h);
    a20_hdl_close(h2);
    a20_path_unlink(A20_HANDLE_NULL, path, (uint32_t)a20_strlen(path));
    return 0;
}

static int handle_transfer_byte_move(void)
{
    const char *src_path = "/tmp/native_xfer_src.txt";
    const char *dst_path = "/tmp/native_xfer_dst.txt";
    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK |
                          A20_RIGHT_TRANSFER;
    a20_handle_t src = open_file(src_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    a20_handle_t dst = open_file(dst_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    if (src == A20_HANDLE_NULL || dst == A20_HANDLE_NULL)
        return fail("xfer-open");

    const char *msg = "native transfer";
    a20_iovec_t wiov = { (uint64_t)msg, a20_strlen(msg) };
    uint64_t written = 0;
    if (a20_status_is_err(a20_hdl_write(src, &wiov, 1, &written)) || written != a20_strlen(msg))
        return fail("xfer-write-src");

    a20_hdl_seek(src, 0, A20_SEEK_START, NULL);

    a20_transfer_args_t targs;
    a20_memset(&targs, 0, sizeof(targs));
    targs.size = sizeof(targs);
    targs.version = 1;
    targs.source = src;
    targs.dest = dst;
    targs.length = a20_strlen(msg);
    targs.flags = 0;
    if (a20_status_is_err(a20_hdl_transfer(&targs)))
        return fail("xfer");
    if (targs.out_transferred != a20_strlen(msg))
        return fail("xfer-count");

    a20_hdl_seek(dst, 0, A20_SEEK_START, NULL);
    char buf[32] = {0};
    a20_iovec_t riov = { (uint64_t)buf, sizeof(buf) - 1 };
    uint64_t got = 0;
    if (a20_status_is_err(a20_hdl_read(dst, &riov, 1, &got)))
        return fail("xfer-read-dst");
    if (got != a20_strlen(msg) || a20_strcmp(buf, msg) != 0)
        return fail("xfer-compare");

    a20_hdl_close(src);
    a20_hdl_close(dst);
    a20_path_unlink(A20_HANDLE_NULL, src_path, (uint32_t)a20_strlen(src_path));
    a20_path_unlink(A20_HANDLE_NULL, dst_path, (uint32_t)a20_strlen(dst_path));
    return 0;
}

static int handle_transfer_peek(void)
{
    const char *src_path = "/tmp/native_peek_src.txt";
    const char *dst_path = "/tmp/native_peek_dst.txt";
    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK |
                          A20_RIGHT_TRANSFER;
    a20_handle_t src = open_file(src_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    a20_handle_t dst = open_file(dst_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    if (src == A20_HANDLE_NULL || dst == A20_HANDLE_NULL)
        return fail("peek-open");

    const char *msg = "peekdata";
    a20_iovec_t wiov = { (uint64_t)msg, a20_strlen(msg) };
    uint64_t written = 0;
    if (a20_status_is_err(a20_hdl_write(src, &wiov, 1, &written)) ||
        written != a20_strlen(msg))
        return fail("peek-write-src");

    a20_hdl_seek(src, 0, A20_SEEK_START, NULL);

    a20_transfer_args_t targs;
    a20_memset(&targs, 0, sizeof(targs));
    targs.size = sizeof(targs);
    targs.version = 1;
    targs.source = src;
    targs.dest = dst;
    targs.length = a20_strlen(msg);
    targs.flags = A20_TRANSFER_PEEK;
    if (a20_status_is_err(a20_hdl_transfer(&targs)))
        return fail("peek");
    if (targs.out_transferred != a20_strlen(msg))
        return fail("peek-count");

    a20_off_t pos = 0;
    if (a20_status_is_err(a20_hdl_seek(src, 0, A20_SEEK_CURRENT, &pos)))
        return fail("peek-seek-cur");
    if (pos != 0)
        return fail("peek-position-changed");

    a20_hdl_close(src);
    a20_hdl_close(dst);
    a20_path_unlink(A20_HANDLE_NULL, src_path, (uint32_t)a20_strlen(src_path));
    a20_path_unlink(A20_HANDLE_NULL, dst_path, (uint32_t)a20_strlen(dst_path));
    return 0;
}

static int handle_transfer_offsets(void)
{
    const char *src_path = "/tmp/native_off_src.txt";
    const char *dst_path = "/tmp/native_off_dst.txt";
    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK |
                          A20_RIGHT_TRANSFER;
    a20_handle_t src = open_file(src_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    a20_handle_t dst = open_file(dst_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    if (src == A20_HANDLE_NULL || dst == A20_HANDLE_NULL)
        return fail("off-open");

    const char *msg = "abcdefghij";
    a20_iovec_t wiov = { (uint64_t)msg, a20_strlen(msg) };
    uint64_t written = 0;
    if (a20_status_is_err(a20_hdl_write(src, &wiov, 1, &written)) ||
        written != a20_strlen(msg))
        return fail("off-write-src");

    a20_hdl_seek(src, 0, A20_SEEK_START, NULL);

    a20_transfer_args_t targs;
    a20_memset(&targs, 0, sizeof(targs));
    targs.size = sizeof(targs);
    targs.version = 1;
    targs.source = src;
    targs.dest = dst;
    targs.source_offset = 2;
    targs.dest_offset = 4;
    targs.length = 4;
    targs.flags = 0;
    if (a20_status_is_err(a20_hdl_transfer(&targs)))
        return fail("off");
    if (targs.out_transferred != 4)
        return fail("off-count");

    a20_hdl_seek(dst, 0, A20_SEEK_START, NULL);
    char buf[32] = {0};
    a20_iovec_t riov = { (uint64_t)buf, sizeof(buf) - 1 };
    uint64_t got = 0;
    if (a20_status_is_err(a20_hdl_read(dst, &riov, 1, &got)))
        return fail("off-read-dst");
    if (got != 8)
        return fail("off-read-len");

    char expected[32] = {0};
    a20_memset(expected, 0, sizeof(expected));
    a20_memcpy(expected + 4, "cdef", 4);
    if (a20_memcmp(buf, expected, 8) != 0)
        return fail("off-compare");

    a20_hdl_close(src);
    a20_hdl_close(dst);
    a20_path_unlink(A20_HANDLE_NULL, src_path, (uint32_t)a20_strlen(src_path));
    a20_path_unlink(A20_HANDLE_NULL, dst_path, (uint32_t)a20_strlen(dst_path));
    return 0;
}

static int handle_transfer_missing_right(void)
{
    const char *src_path = "/tmp/native_miss_src.txt";
    const char *dst_path = "/tmp/native_miss_dst.txt";
    a20_handle_t src = open_file(src_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK);
    a20_handle_t dst = open_file(dst_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK);
    if (src == A20_HANDLE_NULL || dst == A20_HANDLE_NULL)
        return fail("miss-open");

    a20_transfer_args_t targs;
    a20_memset(&targs, 0, sizeof(targs));
    targs.size = sizeof(targs);
    targs.version = 1;
    targs.source = src;
    targs.dest = dst;
    targs.length = 1;
    targs.flags = 0;
    if (a20_status_is_ok(a20_hdl_transfer(&targs)))
        return fail("miss-allowed");

    a20_hdl_close(src);
    a20_hdl_close(dst);
    a20_path_unlink(A20_HANDLE_NULL, src_path, (uint32_t)a20_strlen(src_path));
    a20_path_unlink(A20_HANDLE_NULL, dst_path, (uint32_t)a20_strlen(dst_path));
    return 0;
}

static int handle_transfer_bad_handle(void)
{
    const char *dst_path = "/tmp/native_bad_dst.txt";
    a20_handle_t dst = open_file(dst_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK |
                                 A20_RIGHT_TRANSFER);
    if (dst == A20_HANDLE_NULL)
        return fail("bad-open");

    a20_transfer_args_t targs;
    a20_memset(&targs, 0, sizeof(targs));
    targs.size = sizeof(targs);
    targs.version = 1;
    targs.source = 0xDEADBEEF;
    targs.dest = dst;
    targs.length = 1;
    targs.flags = 0;
    if (a20_status_is_ok(a20_hdl_transfer(&targs)))
        return fail("bad-allowed");

    a20_hdl_close(dst);
    a20_path_unlink(A20_HANDLE_NULL, dst_path, (uint32_t)a20_strlen(dst_path));
    return 0;
}

static int handle_transfer_partial(void)
{
    const char *src_path = "/tmp/native_part_src.txt";
    const char *dst_path = "/tmp/native_part_dst.txt";
    a20_rights_t rights = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK |
                          A20_RIGHT_TRANSFER;
    a20_handle_t src = open_file(src_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    a20_handle_t dst = open_file(dst_path,
                                 A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                                 rights);
    if (src == A20_HANDLE_NULL || dst == A20_HANDLE_NULL)
        return fail("part-open");

    const char *msg = "abc";
    a20_iovec_t wiov = { (uint64_t)msg, a20_strlen(msg) };
    uint64_t written = 0;
    if (a20_status_is_err(a20_hdl_write(src, &wiov, 1, &written)) ||
        written != a20_strlen(msg))
        return fail("part-write-src");

    a20_hdl_seek(src, 0, A20_SEEK_START, NULL);

    a20_transfer_args_t targs;
    a20_memset(&targs, 0, sizeof(targs));
    targs.size = sizeof(targs);
    targs.version = 1;
    targs.source = src;
    targs.dest = dst;
    targs.length = 4096;
    targs.flags = 0;
    if (a20_status_is_err(a20_hdl_transfer(&targs)))
        return fail("part");
    if (targs.out_transferred != a20_strlen(msg))
        return fail("part-count");

    a20_hdl_close(src);
    a20_hdl_close(dst);
    a20_path_unlink(A20_HANDLE_NULL, src_path, (uint32_t)a20_strlen(src_path));
    a20_path_unlink(A20_HANDLE_NULL, dst_path, (uint32_t)a20_strlen(dst_path));
    return 0;
}

/* ---- Typed channel: signature enforcement (docs/native-abi/05-ipc.md §2.3) ---- */

static int typed_channel_enforced(void)
{
    a20_channel_type_t ct;
    a20_memset(&ct, 0, sizeof(ct));
    ct.version = 1;
    ct.send_handle_types = A20_CHAN_TYPE_FILE;
    ct.recv_handle_types = A20_CHAN_TYPE_FILE;

    a20_channel_pair_t pair;
    if (a20_status_is_err(a20_channel_create_typed(&pair, &ct)))
        return fail("tchan-create");

    /* A TIMER handle violates the signature and must be rejected. */
    a20_timer_create_args_t tc;
    a20_memset(&tc, 0, sizeof(tc));
    tc.size = sizeof(tc);
    tc.version = 1;
    tc.event_queue = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_syscall6(A20_SYS_timer_create,
                                       (uint64_t)&tc, 0, 0, 0, 0, 0)))
        return fail("tchan-timer");
    a20_handle_t th = tc.out_timer;

    const char *m = "z";
    a20_status_t r = a20_channel_send(pair.endpoints[0], m, 1, &th, 1);
    if (r != -A20_ERR_TYPE_MISMATCH)
        return fail("tchan-mismatch");

    /* A FILE handle passes the signature and round-trips with rights. */
    const char *path = "/tmp/native_tchan.txt";
    a20_handle_t f = open_file(path,
                               A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                               A20_RIGHT_READ | A20_RIGHT_STAT | A20_RIGHT_TRANSFER);
    if (f == A20_HANDLE_NULL)
        return fail("tchan-open");

    r = a20_channel_send(pair.endpoints[0], m, 1, &f, 1);
    if (a20_status_is_err(r))
        return fail("tchan-send-file");

    char buf[8] = {0};
    uint32_t blen = sizeof(buf);
    a20_handle_t got[1] = { A20_HANDLE_NULL };
    uint32_t gcount = 1;
    r = a20_channel_recv(pair.endpoints[1], buf, &blen, got, &gcount);
    if (a20_status_is_err(r))
        return fail("tchan-recv");
    if (gcount != 1 || got[0] == A20_HANDLE_NULL)
        return fail("tchan-count");

    a20_handle_info_t info;
    a20_memset(&info, 0, sizeof(info));
    if (a20_status_is_err(a20_hdl_query(got[0], &info)))
        return fail("tchan-query");
    if ((info.rights & A20_RIGHT_READ) == 0)
        return fail("tchan-rights");

    a20_hdl_close(got[0]);
    a20_hdl_close(f);
    a20_hdl_close(th);
    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);
    a20_path_unlink(A20_HANDLE_NULL, path, (uint32_t)a20_strlen(path));
    return 0;
}

/* ---- Channel blocking semantics (docs/native-abi/05-ipc.md §2.4-2.5) ---- */

static a20_handle_t g_bch;

static void bch_worker(uint64_t arg)
{
    (void)arg;
    a20_time_t d = { .secs = 0, .nsecs = 30 * 1000 * 1000 };
    a20_thread_sleep(d);
    a20_channel_send(g_bch, "ping", 4, 0, 0);
    a20_thread_exit(0);
}

static int channel_blocking_recv(void)
{
    a20_channel_pair_t pair;
    if (a20_status_is_err(a20_channel_create(&pair)))
        return fail("bch-create");

    /* Empty queue + NONBLOCK must fail immediately. */
    char tmp[4];
    uint32_t tlen = sizeof(tmp);
    a20_handle_t nh[1];
    uint32_t nc = 0;
    if (a20_channel_recv_flags(pair.endpoints[1], tmp, &tlen, nh, &nc,
                               A20_MSG_NONBLOCK) != -A20_ERR_WOULD_BLOCK)
        return fail("bch-nonblock");

    g_bch = pair.endpoints[0];

    uint64_t stack;
    if (a20_vm_alloc_pages(16, 3 /* RW */, &stack) != A20_OK)
        return fail("bch-alloc");

    a20_thread_create_args_t tc = {0};
    tc.entry = (uint64_t)bch_worker;
    tc.arg = 0;
    tc.stack_base = (stack + 16 * 4096) & ~15ULL;
    tc.stack_size = 16 * 4096;
    tc.tls_base = 0;
    if (a20_thread_create(&tc) < 0)
        return fail("bch-thread");

    /* Blocking recv must sleep ~30ms until the worker sends. */
    char buf[8] = {0};
    uint32_t blen = sizeof(buf);
    a20_handle_t hs[1];
    uint32_t hc = 0;
    a20_status_t r = a20_channel_recv(pair.endpoints[1], buf, &blen, hs, &hc);
    if (a20_status_is_err(r))
        return fail("bch-recv");
    if (blen != 4 || a20_memcmp(buf, "ping", 4) != 0)
        return fail("bch-data");

    a20_hdl_close(pair.endpoints[0]);
    a20_hdl_close(pair.endpoints[1]);
    return 0;
}

/* ---- Event queue blocking wait + timer events (05-ipc.md §3) ---- */

static int event_wait_blocks(void)
{
    a20_handle_t eq;
    if (a20_status_is_err(a20_event_queue_create(&eq)))
        return fail("evq-create");

    a20_timer_create_args_t tc;
    a20_memset(&tc, 0, sizeof(tc));
    tc.size = sizeof(tc);
    tc.version = 1;
    tc.event_queue = eq;
    tc.user_data = 0xABCD;
    if (a20_status_is_err(a20_syscall6(A20_SYS_timer_create,
                                       (uint64_t)&tc, 0, 0, 0, 0, 0)))
        return fail("evq-timer");
    a20_handle_t th = tc.out_timer;

    if (a20_status_is_err(a20_event_watch(eq, th,
                                          A20_EVENT_MASK(A20_EVENT_EXPIRED), 0xABCD)))
        return fail("evq-watch");

    /* Poll on the empty queue: WOULD_BLOCK. */
    a20_event_t ev;
    a20_time_t poll = { .secs = 0, .nsecs = 0 };
    if (a20_event_wait(eq, poll, &ev) != -A20_ERR_WOULD_BLOCK)
        return fail("evq-poll");

    /* Arm the timer for now + 100ms, then block up to 2s for the event. */
    uint64_t now = 0;
    if (a20_status_is_err(a20_clock_get(A20_CLOCK_MONOTONIC, &now)))
        return fail("evq-clock");
    if (a20_syscall6(A20_SYS_timer_set, th,
                     now + 100ULL * 1000 * 1000, 0, 0, 0, 0) < 0)
        return fail("evq-arm");

    a20_time_t to = { .secs = 2, .nsecs = 0 };
    a20_status_t r = a20_event_wait(eq, to, &ev);
    if (r < 0)
        return fail("evq-wait");
    if (ev.type != A20_EVENT_EXPIRED || ev.user_data != 0xABCD)
        return fail("evq-event");

    a20_hdl_close(th);
    a20_hdl_close(eq);
    return 0;
}

/* ---- Temporal capabilities: operation budget (03-handle.md §2.6) ---- */

static int temporal_op_count(void)
{
    const char *path = "/tmp/native_opcount.txt";
    a20_handle_t f = open_file(path,
                               A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                               A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK |
                               A20_RIGHT_CONTROL);
    if (f == A20_HANDLE_NULL)
        return fail("opc-open");

    const char *msg = "0123456789";
    a20_iovec_t wiov = { (uint64_t)msg, a20_strlen(msg) };
    uint64_t written = 0;
    if (a20_status_is_err(a20_hdl_write(f, &wiov, 1, &written)))
        return fail("opc-write");
    a20_hdl_seek(f, 0, A20_SEEK_START, NULL);

    /* Budget of 2 operations after this call. */
    if (a20_status_is_err(a20_hdl_set_temporal(f, 0, 2, A20_TEMPORAL_OP_COUNT)))
        return fail("opc-set");

    char b[2];
    uint64_t got = 0;
    a20_iovec_t riov = { (uint64_t)b, 1 };
    if (a20_status_is_err(a20_hdl_read(f, &riov, 1, &got)))
        return fail("opc-read1");
    if (a20_status_is_err(a20_hdl_read(f, &riov, 1, &got)))
        return fail("opc-read2");
    a20_status_t r = a20_hdl_read(f, &riov, 1, &got);
    if (r != -A20_ERR_ACCESS && r != -A20_ERR_EXPIRED)
        return fail("opc-read3");

    a20_path_unlink(A20_HANDLE_NULL, path, (uint32_t)a20_strlen(path));
    return 0;
}

/* ---- Temporal capabilities: expiry + sweeper auto-close (03-handle.md §2.6.4) ---- */

static int temporal_auto_close(void)
{
    const char *path = "/tmp/native_autoclose.txt";
    a20_handle_t f = open_file(path,
                               A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR | A20_PATH_OPEN_TRUNC,
                               A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_CONTROL);
    if (f == A20_HANDLE_NULL)
        return fail("ac-open");

    uint64_t now = 0;
    if (a20_status_is_err(a20_clock_get(A20_CLOCK_MONOTONIC, &now)))
        return fail("ac-clock");
    if (a20_status_is_err(a20_hdl_set_temporal(f, now + 200ULL * 1000 * 1000, 0,
                                               A20_TEMPORAL_EXPIRY_ABSOLUTE |
                                               A20_TEMPORAL_AUTO_CLOSE)))
        return fail("ac-set");

    /* The sweeper runs every ~100ms; 1s is comfortably past the deadline. */
    a20_time_t d = { .secs = 1, .nsecs = 0 };
    a20_thread_sleep(d);

    a20_handle_info_t info;
    a20_memset(&info, 0, sizeof(info));
    if (a20_hdl_query(f, &info) != -A20_ERR_BAD_HANDLE)
        return fail("ac-query");

    a20_path_unlink(A20_HANDLE_NULL, path, (uint32_t)a20_strlen(path));
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

    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "start\n", 6, NULL);

    if (handle_dup_rights_downgrade() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "dup ok\n", 7, NULL);

    if (handle_transfer_byte_move() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "xfer ok\n", 8, NULL);

    if (handle_transfer_peek() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "peek ok\n", 8, NULL);

    if (handle_transfer_offsets() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "offset ok\n", 10, NULL);

    if (handle_transfer_missing_right() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "miss ok\n", 8, NULL);

    if (handle_transfer_bad_handle() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "bad ok\n", 7, NULL);

    if (handle_transfer_partial() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "part ok\n", 8, NULL);

    if (typed_channel_enforced() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "tchan ok\n", 9, NULL);

    if (channel_blocking_recv() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "bch ok\n", 7, NULL);

    if (event_wait_blocks() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "evq ok\n", 7, NULL);

    if (temporal_op_count() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "opc ok\n", 7, NULL);

    if (temporal_auto_close() != 0)
        return 1;
    if (g_stdout != A20_HANDLE_NULL)
        a20_hdl_write_buf(g_stdout, "ac ok\n", 6, NULL);

    return 0;
}

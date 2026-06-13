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

    return 0;
}

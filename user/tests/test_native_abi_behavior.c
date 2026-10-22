/*
 * Normalized ABI behaviour fixture (Native personality).
 *
 * The Native twin of user/cmds/core/abi_behavior.c.  It performs the same
 * operation set in the same order through raw Native primitives
 * (path_open/handle_read/handle_seek/handle_dup/vm_alloc/vm_protect/
 * net_socketpair/personality pipe) and prints the same
 * `ABI_BEHAVIOR: key=value` lines, so the two logs can be diffed directly.
 *
 * The raw primitives are used deliberately: routing this through liba20rt's
 * Linux facade (a20_linux.h) would test the emulation against itself.  The
 * one shared component is the byte-stream pipe primitive in
 * a20_personality.h; that module is the Native answer to pipe(2), not an
 * emulation of the Linux syscall.
 *
 * Error classes are mapped explicitly: the Native ABI has its own status
 * numbers (a20_types.h), so every case prints the shared symbolic class
 * instead of the raw number.
 */
#include "liba20rt/a20_sdk.h"
#include "liba20rt/a20_personality.h"
#include "liba20rt/crt0_a20.h"

static a20_handle_t out = A20_HANDLE_NULL;

static int fail(int code)
{
    a20_hdl_write_buf(out, "ABI_BEHAVIOR: FAIL code=", 24, NULL);
    return code;
}

static int emit(const char *text, uint32_t len)
{
    if (a20_status_is_err(a20_hdl_write_buf(out, "ABI_BEHAVIOR: ", 14, NULL)))
        return 1;
    if (a20_status_is_err(a20_hdl_write_buf(out, text, len, NULL)))
        return 1;
    return a20_status_is_err(a20_hdl_write_buf(out, "\n", 1, NULL)) ? 1 : 0;
}

#define EMIT(literal)                                                        \
    do {                                                                     \
        if (emit(literal, sizeof(literal) - 1))                              \
            return fail(__LINE__);                                           \
    } while (0)

static a20_handle_t open_file(const char *path, uint32_t flags,
                              a20_rights_t rights)
{
    a20_path_open_args_t args;
    a20_memset(&args, 0, sizeof(args));
    args.size = sizeof(args);
    args.version = 1;
    args.dir = A20_HANDLE_NULL;
    args.path = (uint64_t)(uintptr_t)path;
    args.path_len = (uint32_t)a20_strlen(path);
    args.rights = rights;
    args.flags = flags;
    args.mode = 0600;
    args.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_path_open(&args)))
        return A20_HANDLE_NULL;
    return args.out_handle;
}

int main(int argc, char **argv, char **envp)
{
    (void)argc;
    (void)argv;
    (void)envp;
    a20_start_info_t *si = a20_get_start_info();
    out = si ? si->stdout_handle : A20_HANDLE_NULL;
    if (out == A20_HANDLE_NULL)
        return 90;
    EMIT("run=native");

    /* pipe: two writes joined into one byte stream, drained in two reads. */
    a20_personality_pipe_t pipe;
    a20_memset(&pipe, 0, sizeof(pipe));
    pipe.read_end = pipe.write_end = pipe.wait_queue = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_personality_pipe_create(&pipe)))
        return fail(1);
    if (a20_status_is_err(a20_personality_pipe_write(&pipe, "hello ", 6)) ||
        a20_status_is_err(a20_personality_pipe_write(&pipe, "world", 5)))
        return fail(2);
    char joined[16];
    a20_memset(joined, 0, sizeof(joined));
    uint32_t jlen = 6;
    if (a20_status_is_err(a20_personality_pipe_read(&pipe, joined, &jlen)) ||
        jlen != 6)
        return fail(3);
    jlen = sizeof(joined) - 6;
    if (a20_status_is_err(a20_personality_pipe_read(&pipe, joined + 6, &jlen)) ||
        jlen != 5 || a20_memcmp(joined, "hello world", 11) != 0)
        return fail(4);
    a20_personality_pipe_close(&pipe);
    EMIT("pipe partial=6 rest=5 joined=hello world");

    /* fd table: create, write, seek, read back, dup, close-then-use. */
    const char *path = "/tmp/native-abi-behavior.txt";
    a20_handle_t file = open_file(path,
                                  A20_PATH_OPEN_CREATE | A20_PATH_OPEN_RDWR |
                                      A20_PATH_OPEN_TRUNC,
                                  A20_RIGHT_READ | A20_RIGHT_WRITE |
                                      A20_RIGHT_SEEK | A20_RIGHT_DUP |
                                      A20_RIGHT_STAT);
    if (file == A20_HANDLE_NULL)
        return fail(6);
    uint64_t count = 0;
    if (a20_status_is_err(a20_hdl_write_buf(file, "abcdef", 6, &count)) ||
        count != 6)
        return fail(7);
    a20_off_t offset = 0;
    if (a20_status_is_err(a20_hdl_seek(file, 0, A20_SEEK_START, &offset)) ||
        offset != 0)
        return fail(8);
    char fb[16];
    a20_memset(fb, 0, sizeof(fb));
    if (a20_status_is_err(a20_hdl_read_buf(file, fb, 6, &count)) || count != 6 ||
        a20_memcmp(fb, "abcdef", 6) != 0)
        return fail(9);
    a20_handle_dup_args_t dup = {0};
    dup.source = file;
    dup.rights_mask = A20_RIGHT_READ | A20_RIGHT_WRITE | A20_RIGHT_SEEK;
    dup.out_handle = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_hdl_dup(&dup)))
        return fail(10);
    if (a20_status_is_err(a20_hdl_seek(dup.out_handle, 2, A20_SEEK_START,
                                       &offset)) ||
        offset != 2)
        return fail(11);
    char part[8];
    a20_memset(part, 0, sizeof(part));
    if (a20_status_is_err(a20_hdl_read_buf(file, part, 3, &count)) || count != 3 ||
        a20_memcmp(part, "cde", 3) != 0)
        return fail(12);
    a20_hdl_close(dup.out_handle);
    if (a20_status_is_err(a20_hdl_read_buf(file, part, 1, &count)) || count != 1)
        return fail(13);
    a20_handle_t closed = file;
    a20_hdl_close(file);
    if (a20_hdl_read_buf(closed, part, 1, &count) != -A20_ERR_BAD_HANDLE)
        return fail(14);
    EMIT("fd_table write=6 seek=0 read=6 dup_shared_offset=ok "
         "close_then_read=bad_handle");

    /* mmap: anonymous RW page, write and read it, then protect read-only. */
    uint64_t addr = 0;
    if (a20_status_is_err(a20_vm_alloc_pages(1, 3 /* RW */, &addr)))
        return fail(15);
    uint8_t *page = (uint8_t *)(uintptr_t)addr;
    for (int i = 0; i < 4096; i++)
        page[i] = 0x5a;
    if (page[0] != 0x5a || page[4095] != 0x5a)
        return fail(16);
    if (a20_status_is_err(a20_vm_protect(addr, 4096, A20_PROT_READ)) ||
        page[7] != 0x5a)
        return fail(17);
    if (a20_status_is_err(a20_vm_unmap(addr, 4096)))
        return fail(18);
    EMIT("mmap write_read=ok protect_readonly=ok");

    /* socketpair: one message each way. */
    a20_handle_t sv[2];
    if (a20_status_is_err(
            a20_net_socketpair(1 /* AF_UNIX */, 1 /* SOCK_STREAM */, 0, sv)))
        return fail(19);
    if (a20_status_is_err(a20_hdl_write_buf(sv[0], "ping", 4, &count)) ||
        count != 4)
        return fail(20);
    char s[8];
    a20_memset(s, 0, sizeof(s));
    if (a20_status_is_err(a20_hdl_read_buf(sv[1], s, sizeof(s), &count)) ||
        count != 4 || a20_memcmp(s, "ping", 4) != 0)
        return fail(21);
    a20_hdl_close(sv[0]);
    a20_hdl_close(sv[1]);
    EMIT("socketpair send=4 recv=4 data=ping");

    /* Level-triggered readiness: still readable after a partial read. */
    a20_personality_pipe_t stream;
    a20_memset(&stream, 0, sizeof(stream));
    stream.read_end = stream.write_end = stream.wait_queue = A20_HANDLE_NULL;
    if (a20_status_is_err(a20_personality_pipe_create(&stream)))
        return fail(23);
    a20_event_t ev;
    a20_time_t tzero = {.secs = 0, .nsecs = 0};
    a20_time_t timeout = {.secs = 1, .nsecs = 0};
    while (a20_event_wait(stream.wait_queue, tzero, &ev) == 1)
        ; /* drop stale edge events */
    if (a20_status_is_err(a20_personality_pipe_write(&stream, "level", 5)))
        return fail(24);
    if (a20_personality_pipe_wait_readable(&stream, timeout, &ev) != 1)
        return fail(25);
    char lb[8];
    a20_memset(lb, 0, sizeof(lb));
    uint32_t llen = 2;
    if (a20_status_is_err(a20_personality_pipe_read(&stream, lb, &llen)) ||
        llen != 2)
        return fail(26);
    if (a20_personality_pipe_wait_readable(&stream, timeout, &ev) != 1)
        return fail(27); /* still readable: level semantics */
    llen = sizeof(lb);
    if (a20_status_is_err(a20_personality_pipe_read(&stream, lb, &llen)) ||
        llen != 3)
        return fail(28);
    a20_time_t tshort = {.secs = 0, .nsecs = 20 * 1000 * 1000};
    if (a20_personality_pipe_wait_readable(&stream, tshort, &ev) !=
        -A20_ERR_TIMED_OUT)
        return fail(29); /* drained */
    a20_personality_pipe_close(&stream);
    EMIT("readiness level=ok drained=ok");

    /* Invalid-argument class. */
    if (a20_hdl_read_buf(A20_HANDLE_NULL, part, 1, &count) !=
        -A20_ERR_BAD_HANDLE)
        return fail(30);
    uint64_t zero = 0;
    if (a20_vm_alloc_pages(0, 3, &zero) != -A20_ERR_INVALID_ARGUMENT)
        return fail(31);
    EMIT("errors bad_descriptor=bad_handle zero_length=invalid");

    EMIT("PASS");
    return 0;
}

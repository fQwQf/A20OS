/*
 * Linux-ABI extension smoke test: keyring, AIO, acct, fanotify, pidfd.
 *
 * Exercises the syscalls added in the A20OS "complete the Linux ABI" work:
 *   add_key/request_key/keyctl     kernel/ipc/keyring.c
 *   io_setup/io_submit/io_getevents/io_destroy   kernel/fs/aio.c
 *   acct                            kernel/proc/acct.c
 *   fanotify_init/fanotify_mark     kernel/fs/inotify.c
 *   pidfd_open/pidfd_getfd          kernel/abi/linux/sys_pidfd.c
 *
 * Prints SYSCALL_EXT: PASS when every group succeeds.
 */

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

#define KEY_SPEC_PROCESS_KEYRING (-2)
#define KEY_SPEC_SESSION_KEYRING (-3)
#define KEY_SPEC_USER_KEYRING (-4)

#define KEYCTL_GET_KEYRING_ID 0
#define KEYCTL_JOIN_SESSION_KEYRING 1
#define KEYCTL_DESCRIBE 6
#define KEYCTL_READ 11

static int fail(const char *what, int e)
{
    printf("SYSCALL_EXT: FAIL %s errno=%d\n", what, e);
    return -1;
}

static int test_keyring(void)
{
    long session = syscall(SYS_keyctl, KEYCTL_GET_KEYRING_ID,
                           KEY_SPEC_SESSION_KEYRING, 1L, 0L, 0L);
    if (session < 0)
        return fail("keyctl GET_SESSION", (int)session);

    long serial = syscall(SYS_add_key, "user", "a20-test-key", "hello",
                          (size_t)5, KEY_SPEC_PROCESS_KEYRING);
    if (serial < 0)
        return fail("add_key", (int)serial);

    /* Re-add with the same type+description must return the same serial. */
    long serial2 = syscall(SYS_add_key, "user", "a20-test-key", "world",
                           (size_t)5, KEY_SPEC_PROCESS_KEYRING);
    if (serial2 != serial)
        return fail("add_key dedup", 0);

    long found = syscall(SYS_request_key, "user", "a20-test-key", NULL,
                         KEY_SPEC_PROCESS_KEYRING);
    if (found != serial)
        return fail("request_key", (int)found);

    long desc = syscall(SYS_keyctl, KEYCTL_DESCRIBE, serial, 0L, 0L, 0L);
    if (desc < 0)
        return fail("keyctl DESCRIBE", (int)desc);

    char buf[64];
    long n = syscall(SYS_keyctl, KEYCTL_READ, serial, buf,
                     (size_t)sizeof(buf), 0L);
    if (n < 0)
        return fail("keyctl READ", (int)n);
    /* The dedup re-add above updated the payload to "world". */
    if (n == 5 && memcmp(buf, "world", 5) != 0)
        return fail("keyctl READ content", 0);

    printf("SYSCALL_EXT: keyring ok (session=%ld serial=%ld desc=%ld)\n",
           session, serial, desc);
    return 0;
}

/* io_event_test.c already covers blocking I/O; here we validate the AIO ring
 * submission/completion contract end to end. */
struct linux_iocb {
    uint64_t aio_data;
    uint32_t aio_key;
    uint32_t aio_reserved1;
    uint16_t aio_lio_opcode;
    int16_t  aio_reqprio;
    uint32_t aio_fildes;
    uint64_t aio_buf;
    uint64_t aio_nbytes;
    int64_t  aio_offset;
    uint64_t aio_reserved2;
    uint32_t aio_flags;
    uint32_t aio_resfd;
};

struct linux_io_event {
    uint64_t data;
    uint64_t obj;
    int64_t  res;
    int64_t  res2;
};

struct linux_timespec {
    int64_t tv_sec;
    int64_t tv_nsec;
};

#define IOCB_CMD_PREAD 0
#define IOCB_CMD_PWRITE 1

static int test_aio(void)
{
    const char *path = "/tmp/syscall_ext_aio.txt";
    const char *payload = "aio-payload-1234";
    size_t plen = strlen(payload);
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0644);
    if (fd < 0)
        return fail("aio open", errno);
    if (write(fd, payload, plen) != (ssize_t)plen) {
        close(fd);
        return fail("aio write", errno);
    }

    unsigned long ctx = 0;
    if (syscall(SYS_io_setup, 64, &ctx) != 0)
        return fail("io_setup", errno);
    if (ctx == 0)
        return fail("io_setup ctx", 0);

    char rbuf[64];
    memset(rbuf, 0, sizeof(rbuf));
    struct linux_iocb iocb;
    memset(&iocb, 0, sizeof(iocb));
    iocb.aio_data = 0x1234;
    iocb.aio_lio_opcode = IOCB_CMD_PREAD;
    iocb.aio_fildes = (uint32_t)fd;
    iocb.aio_buf = (uint64_t)(uintptr_t)rbuf;
    iocb.aio_nbytes = plen;
    iocb.aio_offset = 0;

    struct linux_iocb *iocbs[1] = { &iocb };
    long submitted = syscall(SYS_io_submit, ctx, 1, iocbs);
    if (submitted != 1)
        return fail("io_submit", (int)submitted);

    struct linux_io_event ev;
    struct linux_timespec ts = { 2, 0 };
    long got = syscall(SYS_io_getevents, ctx, 1, 1, &ev, &ts);
    if (got != 1)
        return fail("io_getevents", (int)got);
    if (ev.data != 0x1234)
        return fail("io_getevents data", 0);
    if (ev.res != (int64_t)plen)
        return fail("io_getevents res", (int)ev.res);
    if (memcmp(rbuf, payload, plen) != 0)
        return fail("io_getevents buffer", 0);

    if (syscall(SYS_io_destroy, ctx) != 0)
        return fail("io_destroy", errno);

    close(fd);
    unlink(path);
    printf("SYSCALL_EXT: aio ok\n");
    return 0;
}

static int test_acct(void)
{
    /* acct("/tmp/acct.log") should enable accounting; the file must be
     * created and the syscall return 0. */
    const char *path = "/tmp/syscall_ext_acct.log";
    if (syscall(SYS_acct, path) != 0)
        return fail("acct enable", errno);
    struct stat st;
    if (stat(path, &st) != 0)
        return fail("acct file", errno);
    if (syscall(SYS_acct, NULL) != 0)
        return fail("acct disable", errno);
    unlink(path);
    printf("SYSCALL_EXT: acct ok\n");
    return 0;
}

static int test_fanotify(void)
{
    int ffd = syscall(SYS_fanotify_init, 0, 0);
    if (ffd < 0)
        return fail("fanotify_init", errno);

    const char *dir = "/tmp";
    long mark_ret = syscall(SYS_fanotify_mark, ffd, 1 /* FAN_MARK_ADD */,
                0x2 /* IN_MODIFY */, AT_FDCWD, dir);
    if (mark_ret != 0) {
        printf("SYSCALL_EXT: fanotify_mark returned %ld errno=%d\n",
               mark_ret, errno);
        return fail("fanotify_mark", errno);
    }

    close(ffd);
    printf("SYSCALL_EXT: fanotify ok\n");
    return 0;
}

static int test_pidfd(void)
{
    int pidfd = syscall(SYS_pidfd_open, (int)getpid(), 0);
    if (pidfd < 0)
        return fail("pidfd_open", errno);

    /* pidfd_getfd of our own stdout must succeed. */
    int dupfd = syscall(SYS_pidfd_getfd, pidfd, 1, 0);
    if (dupfd < 0)
        return fail("pidfd_getfd", errno);
    if (write(dupfd, "SYSCALL_EXT: pidfd ok\n", 22) != 22)
        return fail("pidfd_getfd write", errno);
    close(dupfd);
    close(pidfd);
    return 0;
}

int main(void)
{
    printf("SYSCALL_EXT: start\n");
    if (test_keyring() < 0)
        return 1;
    if (test_aio() < 0)
        return 1;
    if (test_acct() < 0)
        return 1;
    if (test_fanotify() < 0)
        return 1;
    if (test_pidfd() < 0)
        return 1;
    printf("SYSCALL_EXT: PASS\n");
    return 0;
}

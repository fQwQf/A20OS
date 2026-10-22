/* Exercise a real ufsd/ext4 transaction through a one-shot checkpoint flush
 * failure, a live retry, and service death/remount persistence. This binary
 * is installed only in the dedicated failure-injection guest image. */
#define _GNU_SOURCE
#include "../../liba20rt/a20_arch.h"
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int fail(const char *why)
{
    printf("JBD2_RETRY: FAIL %s errno=%d\n", why, errno);
    return 1;
}

static pid_t start_ufsd(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl(A20_NATIVE_BIN("ufsd"), "ufsd", "/uext4", "2", "ext4",
              (char *)0);
        _exit(90);
    }
    return pid;
}

static int wait_mount(void)
{
    struct stat st;
    for (int i = 0; i < 500; i++) {
        if (stat("/uext4/hello.txt", &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

static int remount_after_kill(pid_t pid)
{
    kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
    if (umount2("/uext4", 0) != 0)
        return -1;
    if (start_ufsd() < 0)
        return -1;
    return wait_mount();
}

int main(int argc, char **argv)
{
    int crash_before_retry = argc > 1 && strcmp(argv[1], "crash") == 0;
    int verify_replay = argc > 1 && strcmp(argv[1], "verify") == 0;
    static const char marker[] = "checkpoint-retry-survived\n";
    pid_t service = start_ufsd();
    if (service < 0 || wait_mount() != 0)
        return fail("mount");

    if (verify_replay) {
        int fd = open("/uext4/jbd2-retry.bin", O_RDONLY);
        if (fd < 0)
            return fail("open after crash recovery");
        char got[sizeof(marker)] = {0};
        ssize_t n = read(fd, got, sizeof(got) - 1);
        close(fd);
        if (n != (ssize_t)(sizeof(marker) - 1) ||
            memcmp(got, marker, sizeof(marker) - 1) != 0)
            return fail("committed content after recovery");
        printf("JBD2_REPLAY: PASS committed content after cold remount\n");
        execl("/bin/poweroff", "poweroff", (char *)0);
        return 0;
    }

    int fd = open("/uext4/jbd2-retry.bin", O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return fail("open");
    if (write(fd, marker, sizeof(marker) - 1) != (ssize_t)(sizeof(marker) - 1))
        return fail("write");
    errno = 0;
    int first_sync = fsync(fd);
    printf("JBD2_RETRY: first fsync rc=%d errno=%d\n", first_sync, errno);
    if (first_sync == 0 || errno != EIO)
        return fail("first fsync did not report injected EIO");
    if (crash_before_retry) {
        /* The transaction is committed in the journal, but the injected
         * checkpoint barrier failed. Kill the service and power off without
         * close/unmount/retry; a second boot must recover from s_start. */
        kill(service, SIGKILL);
        waitpid(service, NULL, 0);
        printf("JBD2_REPLAY: CRASH first-fsync=EIO killed-before-retry\n");
        execl("/bin/poweroff", "poweroff", (char *)0);
        return 0;
    }
    if (fsync(fd) != 0)
        return fail("checkpoint retry");
    if (close(fd) != 0)
        return fail("close");

    if (remount_after_kill(service) != 0)
        return fail("dead service unmount/remount");
    fd = open("/uext4/jbd2-retry.bin", O_RDONLY);
    if (fd < 0)
        return fail("open after remount");
    char got[sizeof(marker)] = {0};
    ssize_t n = read(fd, got, sizeof(got) - 1);
    close(fd);
    if (n != (ssize_t)(sizeof(marker) - 1) ||
        memcmp(got, marker, sizeof(marker) - 1) != 0)
        return fail("content after remount");
    printf("JBD2_RETRY: PASS first-fsync=EIO retry=ok remount=ok\n");
    return 0;
}

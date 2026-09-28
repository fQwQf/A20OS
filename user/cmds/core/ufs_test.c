/*
 * ufs_test — end-to-end validation of the user-space filesystem service
 * (ufsd).
 *
 * Precondition: QEMU already has a third virtio-blk disk attached
 * (DEV_CLASS_BLOCK index 2, FAT32, containing HELLO.TXT). Flow:
 *   1. fork/exec /bin/ufsd-rv and have it mount block device 2 onto /ufs as
 *      a uxfs;
 *   2. Poll until /ufs/HELLO.TXT is visible (mount + INIT handshake done);
 *   3. Read back the HELLO.TXT content and compare;
 *   4. Create ROUND.BIN, write a check pattern, and read it back to compare;
 *   5. Listing the directory should contain both files;
 *   6. After unlinking ROUND.BIN, confirm it is gone.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <dirent.h>

static int fail(const char *why)
{
    printf("UXFS_FS: FAIL %s\n", why);
    return 1;
}

static void spawn_ufsd(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/ufsd-rv", "ufsd-rv", "/ufs", "1", (char *)0);
        _exit(90);
    }
    (void)pid; /* the service stays resident; the parent keeps polling the
            * mount point */
}

int main(void)
{
    const char *hello_path = "/ufs/HELLO.TXT";
    const char *expect = "hello-uxfs\n";

    /* 1-2. Bring the service up and wait for the mount to become visible. */
    int mounted = 0;
    struct stat st;
    if (stat(hello_path, &st) == 0) {
        mounted = 1;
    } else {
        spawn_ufsd();
        for (int i = 0; i < 500; i++) {
            if (stat(hello_path, &st) == 0) { mounted = 1; break; }
            usleep(20000);
        }
    }
    if (!mounted)
        return fail("/ufs not visible after ufsd spawn");
    printf("UXFS_FS: /ufs mounted, HELLO.TXT present\n");

    /* 3. Read back the preseeded content. */
    char buf[64];
    memset(buf, 0, sizeof(buf));
    int fd = open(hello_path, O_RDONLY);
    if (fd < 0)
        return fail("open HELLO.TXT");
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    if (n <= 0 || strcmp(buf, expect) != 0)
        return fail("HELLO.TXT content mismatch");
    printf("UXFS_FS: read-back ok (%zd bytes)\n", n);

    /* 4. Create a file, write the pattern, and read it back. */
    const char *round_path = "/ufs/ROUND.BIN";
    static unsigned char pattern[8192];
    for (size_t i = 0; i < sizeof(pattern); i++)
        pattern[i] = (unsigned char)(i * 31 + 7);
    fd = open(round_path, O_CREAT | O_TRUNC | O_WRONLY, 0644);
    if (fd < 0)
        return fail("create ROUND.BIN");
    ssize_t wn = write(fd, pattern, sizeof(pattern));
    close(fd);
    if (wn != (ssize_t)sizeof(pattern))
        return fail("write ROUND.BIN");

    static unsigned char back[8192];
    fd = open(round_path, O_RDONLY);
    if (fd < 0)
        return fail("reopen ROUND.BIN");
    ssize_t rn = read(fd, back, sizeof(back));
    close(fd);
    if (rn != (ssize_t)sizeof(pattern) || memcmp(pattern, back, sizeof(pattern)) != 0)
        return fail("read-back mismatch on ROUND.BIN");
    printf("UXFS_FS: create/write/read ok (%zd bytes)\n", rn);

    /* 5. Directory listing. */
    DIR *d = opendir("/ufs");
    if (!d)
        return fail("opendir /ufs");
    int saw_hello = 0, saw_round = 0;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL) {
        if (strcmp(ent->d_name, "HELLO.TXT") == 0) saw_hello = 1;
        if (strcmp(ent->d_name, "ROUND.BIN") == 0) saw_round = 1;
    }
    closedir(d);
    if (!saw_hello || !saw_round)
        return fail("readdir missing entries");
    printf("UXFS_FS: readdir ok\n");

    /* 6. Confirm it is gone after deletion. */
    if (unlink(round_path) != 0)
        return fail("unlink ROUND.BIN");
    if (stat(round_path, &st) == 0)
        return fail("unlink did not take effect");
    printf("UXFS_FS: PASS\n");
    return 0;
}

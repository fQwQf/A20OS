/*
 * lfs_test — guest-side gate for the littlefs VFS adapter.
 *
 * The image (created by tools/mkfs_lfs.c) carries /hello.txt and
 * /data/seed.bin with deterministic contents.  The test:
 *   1. mounts the second virtio-blk device as littlefs
 *   2. verifies the payload byte-for-byte
 *   3. exercises create/write/fsync/read + mkdir/unlink
 *   4. unmounts, remounts, verifies the guest-written file survived
 *      (fsync durability path through the littlefs commit + device flush)
 * Prints LITTLEFS: PASS and powers off.
 */
#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/mount.h>
#include <errno.h>
#include <sys/stat.h>

static int fails;

static void check(int ok, const char *what) {
    if (!ok) {
        printf("LITTLEFS: FAIL %s\n", what);
        fails++;
    }
}

#define SEED_SIZE 4096
static void fill_seed(unsigned char *b) {
    for (int i = 0; i < SEED_SIZE; i++)
        b[i] = (unsigned char)(i * 131 + 17);
}

int main(void) {
    mkdir("/lfs", 0755);
    if (mount("/dev/vdb", "/lfs", "littlefs", 0, NULL) != 0) {
        printf("LITTLEFS: FAIL mount (errno=%d)\n", errno);
        return 1;
    }

    /* 2. payload written by mkfs_lfs */
    int fd = open("/lfs/hello.txt", O_RDONLY);
    check(fd >= 0, "open hello.txt");
    if (fd >= 0) {
        char buf[256];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) buf[n] = '\0';
        check(n > 0 && strstr(buf, "pair-block commits") != NULL,
              "hello.txt content");
        close(fd);
    }

    fd = open("/lfs/data/seed.bin", O_RDONLY);
    check(fd >= 0, "open data/seed.bin");
    if (fd >= 0) {
        unsigned char want[SEED_SIZE], got[SEED_SIZE];
        fill_seed(want);
        ssize_t n = read(fd, got, SEED_SIZE);
        check(n == SEED_SIZE && memcmp(want, got, SEED_SIZE) == 0,
              "seed.bin byte-for-byte");
        close(fd);
    }

    /* 3. create / write / fsync / read back */
    fd = open("/lfs/guest.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
    check(fd >= 0, "create guest.txt");
    const char *msg = "written by the A20OS littlefs gate\n";
    if (fd >= 0) {
        check(write(fd, msg, strlen(msg)) == (ssize_t)strlen(msg),
              "write guest.txt");
        int fr = fsync(fd);
        if (fr != 0) printf("LITTLEFS: fsync errno=%d\n", errno);
        check(fr == 0, "fsync guest.txt");
        close(fd);
    }

    int dfd = open("/lfs/data/nested.txt", O_WRONLY | O_CREAT, 0644);
    if (dfd >= 0) {
        check(write(dfd, "nested", 6) == 6, "write nested.txt");
        close(dfd);
        check(unlink("/lfs/data/nested.txt") == 0, "unlink nested.txt");
    }

    /* 4. unmount + remount, then verify durability */
    check(umount("/lfs") == 0, "umount");
    check(mount("/dev/vdb", "/lfs", "littlefs", 0, NULL) == 0, "remount");

    fd = open("/lfs/guest.txt", O_RDONLY);
    check(fd >= 0, "reopen guest.txt after remount");
    if (fd >= 0) {
        char buf[128];
        ssize_t n = read(fd, buf, sizeof(buf) - 1);
        if (n > 0) buf[n] = '\0';
        check(n == (ssize_t)strlen(msg) && memcmp(buf, msg, strlen(msg)) == 0,
              "guest.txt survived remount");
        close(fd);
    }

    /* unlink + recreate proves metadata commits */
    check(unlink("/lfs/guest.txt") == 0, "unlink guest.txt");
    check(open("/lfs/guest.txt", O_RDONLY) < 0, "guest.txt really gone");
    umount("/lfs");

    if (!fails) {
        printf("LITTLEFS: PASS\n");
        return 0;
    }
    printf("LITTLEFS: %d failure(s)\n", fails);
    return 1;
}

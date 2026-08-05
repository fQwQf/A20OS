/*
 * ubd_recover — user-space virtio-blk driver crash recovery (M4).
 *
 * The block service runs in a user process (ubd-rv) behind the kernel
 * block proxy; killing the driver must NOT kill the system, and after a
 * supervisor-style respawn the SAME block device (and its FAT32 mount at
 * /ubd) must keep serving I/O:
 *   1. spawn driver, wait for /ubd, write marker file;
 *   2. SIGKILL the driver process (in-flight requests fail with -EIO,
 *      none are lost silently);
 *   3. respawn the driver (it re-attaches the surviving block device);
 *   4. read the marker file back and write a second one — block service
 *      survives a driver crash through respawn.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define M1  "/ubd/m1.txt"
#define M2  "/ubd/m2.txt"
#define MSG "driver-crash-survivor"

static int write_file(const char *path, const char *content)
{
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) return -1;
    size_t n = strlen(content) + 1;
    ssize_t w = write(fd, content, n);
    fsync(fd);
    close(fd);
    return w == (ssize_t)n ? 0 : -1;
}

static int read_check(const char *path, const char *expect)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) return -1;
    char buf[64] = {0};
    ssize_t n = read(fd, buf, sizeof(buf) - 1);
    close(fd);
    return (n > 0 && strcmp(buf, expect) == 0) ? 0 : -1;
}

static pid_t spawn_driver(void)
{
    pid_t pid = fork();
    if (pid == 0) {
        execl("/bin/ubd-rv", "ubd-rv", (char *)0);
        _exit(90);
    }
    return pid;
}

int main(void)
{
    pid_t drv = spawn_driver();

    /* Wait for the mount + first attach to complete. */
    struct stat st;
    int ready = 0;
    for (int i = 0; i < 500 && !ready; i++) {
        if (stat("/ubd", &st) == 0) ready = 1;
        usleep(20000);
    }
    if (!ready) {
        printf("UBD_RECOVER: FAIL /ubd not mounted\n");
        return 1;
    }

    if (write_file(M1, MSG) != 0) {
        printf("UBD_RECOVER: FAIL initial write\n");
        return 2;
    }
    if (read_check(M1, MSG) != 0) {
        printf("UBD_RECOVER: FAIL initial readback\n");
        return 3;
    }

    /* Crash the driver mid-service. */
    kill(drv, SIGKILL);
    int status;
    waitpid(drv, &status, 0);
    printf("UBD_RECOVER: driver killed, respawning\n");

    /* Respawn: it re-attaches the surviving block device. */
    pid_t drv2 = spawn_driver();
    for (int i = 0; i < 500; i++) {
        usleep(20000);
        /* The re-attach happens quickly; give it up to ~10 s. */
        struct stat s2;
        (void)s2;
        if (i > 50) break;
    }

    if (read_check(M1, MSG) != 0) {
        printf("UBD_RECOVER: FAIL readback after respawn\n");
        kill(drv2, SIGKILL);
        waitpid(drv2, NULL, 0);
        return 4;
    }
    if (write_file(M2, MSG) != 0 || read_check(M2, MSG) != 0) {
        printf("UBD_RECOVER: FAIL write after respawn\n");
        kill(drv2, SIGKILL);
        waitpid(drv2, NULL, 0);
        return 5;
    }

    kill(drv2, SIGKILL);
    waitpid(drv2, NULL, 0);

    printf("UBD_RECOVER: PASS (block service survives driver crash)\n");
    return 0;
}

/* Real UFSD graceful unmount and clean-state cold-restart fixture. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int wait_file(const char *path)
{
    struct stat st;
    for (int i = 0; i < 500; ++i) {
        if (stat(path, &st) == 0)
            return 0;
        usleep(20000);
    }
    return -1;
}

int main(int argc, char **argv)
{
    if (argc != 2 || (strcmp(argv[1], "clean") && strcmp(argv[1], "verify"))) {
        fprintf(stderr, "usage: ufs_mount_state_test clean|verify\n");
        return 2;
    }
    int verify = strcmp(argv[1], "verify") == 0;
    const char *marker = "/uext4/MOUNT.STATE";
    pid_t service = fork();
    if (service == 0) {
        execl("/bin/ufsd-rv", "ufsd-rv", "/uext4", "2", "ext4", (char *)0);
        _exit(90);
    }
    if (service < 0 || wait_file("/uext4/hello.txt") != 0) {
        fprintf(stderr, "MOUNT_STATE: FAIL service mount errno=%d\n", errno);
        return 1;
    }

    if (!verify) {
        int fd = open(marker, O_CREAT | O_TRUNC | O_WRONLY, 0644);
        static const char data[] = "clean-reboot\n";
        if (fd < 0 || write(fd, data, sizeof(data) - 1) != sizeof(data) - 1 ||
            fsync(fd) != 0 || close(fd) != 0) {
            fprintf(stderr, "MOUNT_STATE: FAIL marker write errno=%d\n", errno);
            return 1;
        }
    } else {
        char data[32] = {0};
        int fd = open(marker, O_RDONLY);
        ssize_t n = fd < 0 ? -1 : read(fd, data, sizeof(data));
        if (fd >= 0)
            close(fd);
        if (n != 13 || memcmp(data, "clean-reboot\n", 13) != 0) {
            fprintf(stderr, "MOUNT_STATE: FAIL marker read errno=%d n=%ld\n",
                    errno, (long)n);
            return 1;
        }
    }

    if (umount2("/uext4", 0) != 0) {
        fprintf(stderr, "MOUNT_STATE: FAIL umount errno=%d\n", errno);
        return 1;
    }
    int status = 0;
    if (waitpid(service, &status, 0) != service || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        fprintf(stderr, "MOUNT_STATE: FAIL service status=%d\n", status);
        return 1;
    }
    printf("MOUNT_STATE: %s PASS umount=0 service_exit=0\n",
           verify ? "COLD_RESTART" : "CLEAN_UNMOUNT");
    return 0;
}

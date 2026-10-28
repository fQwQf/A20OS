/*
 * svcctl — service checkpoint/restore control tool (CRIU-lite front end).
 *
 *   svcctl checkpoint <pid> <file>   freeze task and dump A20CKPT1 image
 *   svcctl restore <file>            become the checkpointed task (exec-style)
 *
 * Both map to the A20 Linux-ABI bridge syscalls; the kernel implementation
 * lives in kernel/proc/checkpoint.c and kernel/proc/ckpt_restore.c.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/syscall.h>
#include <unistd.h>

#define SYS_A20_TASK_CHECKPOINT 923
#define SYS_A20_TASK_RESTORE   924

static void usage(void)
{
    fprintf(stderr,
            "usage: svcctl checkpoint <pid> <file>\n"
            "       svcctl restore <file>\n");
}

int main(int argc, char **argv)
{
    if (argc == 4 && !strcmp(argv[1], "checkpoint")) {
        int pid = atoi(argv[2]);
        int fd = open(argv[3], O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (fd < 0) {
            perror("svcctl: open output");
            return 1;
        }
        long rc = syscall(SYS_A20_TASK_CHECKPOINT, pid, fd, 0);
        int saved = errno;
        close(fd);
        if (rc < 0) {
            errno = saved;
            perror("svcctl: checkpoint");
            return 1;
        }
        printf("svcctl: checkpointed pid=%d -> %s\n", pid, argv[3]);
        return 0;
    }
    if (argc == 3 && !strcmp(argv[1], "restore")) {
        int fd = open(argv[2], O_RDONLY);
        if (fd < 0) {
            perror("svcctl: open image");
            return 1;
        }
        /* Success replaces this process entirely; only failure returns. */
        syscall(SYS_A20_TASK_RESTORE, fd);
        perror("svcctl: restore");
        return 1;
    }
    usage();
    return 2;
}

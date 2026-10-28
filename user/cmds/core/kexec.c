#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/reboot.h>
#include <sys/syscall.h>

#define KEXEC_FILE_UNLOAD 1UL

static void usage(void)
{
    fprintf(stderr,
            "usage: kexec -l kernel.elf  load an image\n"
            "       kexec kernel.elf      load and execute\n"
            "       kexec -e              execute loaded image\n"
            "       kexec -d              discard loaded image\n");
}

static int load_image(const char *path)
{
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror("kexec: open");
        return -1;
    }
    long rc = syscall(SYS_kexec_file_load, fd, -1, 0, NULL, 0);
    int saved = errno;
    close(fd);
    if (rc < 0) {
        errno = saved;
        perror("kexec: kexec_file_load");
        return -1;
    }
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "-d")) {
        if (syscall(SYS_kexec_file_load, -1, -1, 0, NULL, KEXEC_FILE_UNLOAD) < 0) {
            perror("kexec: unload");
            return 1;
        }
        return 0;
    }
    if (argc == 2 && !strcmp(argv[1], "-e")) {
        puts("kexec: executing loaded kernel");
        fflush(stdout);
        if (reboot(RB_KEXEC) < 0) {
            perror("kexec: reboot");
            return 1;
        }
        return 0;
    }
    int load_only = argc == 3 && !strcmp(argv[1], "-l");
    const char *path = load_only ? argv[2] : argc == 2 ? argv[1] : NULL;
    if (!path || path[0] == '-') {
        usage();
        return 2;
    }
    if (load_image(path) < 0)
        return 1;
    puts("kexec: image staged (Linux ABI)");
    if (load_only)
        return 0;
    puts("kexec: executing loaded kernel");
    fflush(stdout);
    if (reboot(RB_KEXEC) < 0) {
        perror("kexec: reboot");
        return 1;
    }
    return 0;
}

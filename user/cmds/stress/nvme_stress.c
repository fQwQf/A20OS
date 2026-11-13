/*
 * NVMe read stress: N 4 KiB reads from a block device, default 4096 reads
 * (16 MiB).  One command per read with the driver's default queue depth of 64
 * crosses the completion-queue wrap after 64 reads, so a short count is a
 * direct wrap test:
 *
 *   nvme_stress /dev/disk0 32    # below one wrap
 *   nvme_stress /dev/disk0 65    # first wrap plus one
 */
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    const char *path = argc > 1 ? argv[1] : "/dev/disk0";
    long reads = argc > 2 ? strtol(argv[2], NULL, 0) : 4096;
    size_t io = argc > 3 ? (size_t)strtoul(argv[3], NULL, 0) : 4096;
    if (io == 0 || io > 16384) {
        fprintf(stderr, "size must be 1..16384\n");
        return 1;
    }
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        perror(path);
        return 1;
    }
    static char buf[16384];
    for (long i = 0; i < reads; i++) {
        off_t off = (off_t)(i % 1024) * (off_t)io;
        if (lseek(fd, off, SEEK_SET) < 0) {
            perror("lseek");
            return 1;
        }
        ssize_t n = read(fd, buf, io);
        if (n != (ssize_t)io) {
            perror("read");
            return 1;
        }
    }
    puts("nvme_stress ok");
    close(fd);
    return 0;
}

/* Real file-backed mmap used by the JamVM ZIP bootstrap loader. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
int main(int argc, char **argv) {
    const char *path = argc > 1 ? argv[1] : "/bin/classes.zip";
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("MLIBC_FILEMAP: FAIL open errno=%d\n", errno);
        return 1;
    }
    struct stat info;
    if (fstat(fd, &info) || info.st_size < 4) {
        printf("MLIBC_FILEMAP: FAIL stat errno=%d size=%ld\n", errno,
               (long)info.st_size);
        return 2;
    }
    unsigned char sample[4096];
    size_t count = info.st_size < (off_t)sizeof(sample) ? (size_t)info.st_size
                                                        : sizeof(sample);
    if (pread(fd, sample, count, 0) != (ssize_t)count) {
        printf("MLIBC_FILEMAP: FAIL pread errno=%d\n", errno);
        return 3;
    }
    void *map = mmap(NULL, info.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    if (map == MAP_FAILED) {
        printf("MLIBC_FILEMAP: FAIL mmap errno=%d size=%ld\n", errno,
               (long)info.st_size);
        return 4;
    }
    if (memcmp(sample, map, count) || memcmp(map, "PK\003\004", 4)) {
        puts("MLIBC_FILEMAP: FAIL mapped bytes");
        return 5;
    }
    if (close(fd) || memcmp(sample, map, count) || munmap(map, info.st_size)) {
        printf("MLIBC_FILEMAP: FAIL lifetime errno=%d\n", errno);
        return 6;
    }
    puts("MLIBC_FILEMAP: PASS real ZIP mapping bytes fd-close unmap");
    return 0;
}

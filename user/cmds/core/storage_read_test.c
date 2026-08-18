#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define SAMPLE_SIZE (4U * 1024U)
#define STREAM_CHUNK (1024U * 1024U)
#define PROGRESS_STEP (1024ULL * 1024ULL * 1024ULL)
#define FOUR_GIB (4ULL * 1024ULL * 1024ULL * 1024ULL)

static uint64_t sparse_digest(uint64_t digest, const uint8_t *buf, size_t len)
{
    for (size_t off = 0; off < len; off += SAMPLE_SIZE) {
        size_t tail = len - off;
        size_t take = tail < sizeof(uint64_t) ? tail : sizeof(uint64_t);
        uint64_t word = 0;
        memcpy(&word, buf + off, take);
        digest ^= word + 0x9e3779b97f4a7c15ULL +
                  (digest << 6) + (digest >> 2);
    }
    return digest;
}

static int check_sample(int fd, uint64_t offset, uint64_t size,
                        uint8_t *buf)
{
    size_t wanted = size - offset < SAMPLE_SIZE ?
                    (size_t)(size - offset) : SAMPLE_SIZE;
    ssize_t got = pread(fd, buf, wanted, (off_t)offset);
    if (got != (ssize_t)wanted) {
        printf("STORAGE_READ: FAIL pread offset=%llu wanted=%zu got=%ld errno=%d\n",
               (unsigned long long)offset, wanted, (long)got, errno);
        return -1;
    }
    uint64_t digest = sparse_digest(0, buf, wanted);
    printf("STORAGE_READ: sample offset=%llu bytes=%zu digest=%llx\n",
           (unsigned long long)offset, wanted,
           (unsigned long long)digest);
    return 0;
}

int main(int argc, char **argv)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    if (argc < 2 || argc > 3) {
        printf("usage: storage_read_test FILE [sample]\n");
        return 2;
    }

    int write_fd = open(argv[1], O_WRONLY);
    if (write_fd >= 0) {
        close(write_fd);
        printf("STORAGE_READ: FAIL write-open unexpectedly succeeded\n");
        return 1;
    }
    if (errno != EROFS) {
        printf("STORAGE_READ: FAIL write-open errno=%d expected=%d\n",
               errno, EROFS);
        return 1;
    }
    printf("STORAGE_READ: write fence PASS (EROFS)\n");

    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) {
        printf("STORAGE_READ: FAIL open errno=%d\n", errno);
        return 1;
    }
    struct stat st;
    memset(&st, 0, sizeof(st));
    if (fstat(fd, &st) < 0 || st.st_size <= 0) {
        printf("STORAGE_READ: FAIL stat errno=%d size=%lld\n", errno,
               (long long)st.st_size);
        close(fd);
        return 1;
    }
    uint64_t size = (uint64_t)st.st_size;
    printf("STORAGE_READ: size=%llu bytes\n", (unsigned long long)size);

    uint8_t *buf = malloc(STREAM_CHUNK);
    if (!buf) {
        printf("STORAGE_READ: FAIL allocate %u bytes\n", STREAM_CHUNK);
        close(fd);
        return 1;
    }

    uint64_t offsets[4];
    size_t samples = 0;
    offsets[samples++] = 0;
    if (size > FOUR_GIB) {
        offsets[samples++] = FOUR_GIB - SAMPLE_SIZE;
        offsets[samples++] = FOUR_GIB;
    }
    if (size > SAMPLE_SIZE) {
        uint64_t tail = size - SAMPLE_SIZE;
        if (tail != offsets[samples - 1])
            offsets[samples++] = tail;
    }
    for (size_t i = 0; i < samples; i++) {
        if (check_sample(fd, offsets[i], size, buf) < 0) {
            free(buf);
            close(fd);
            return 1;
        }
    }
    if (argc == 3 && strcmp(argv[2], "sample") == 0) {
        printf("STORAGE_READ: SAMPLE PASS\n");
        free(buf);
        close(fd);
        return 0;
    }

    if (lseek(fd, 0, SEEK_SET) < 0) {
        printf("STORAGE_READ: FAIL seek errno=%d\n", errno);
        free(buf);
        close(fd);
        return 1;
    }
    uint64_t total = 0;
    uint64_t next_progress = PROGRESS_STEP;
    uint64_t digest = 0;
    while (total < size) {
        size_t wanted = size - total < STREAM_CHUNK ?
                        (size_t)(size - total) : STREAM_CHUNK;
        ssize_t got = read(fd, buf, wanted);
        if (got <= 0) {
            printf("STORAGE_READ: FAIL stream offset=%llu got=%ld errno=%d\n",
                   (unsigned long long)total, (long)got, errno);
            free(buf);
            close(fd);
            return 1;
        }
        digest = sparse_digest(digest, buf, (size_t)got);
        total += (uint64_t)got;
        if (total >= next_progress || total == size) {
            printf("STORAGE_READ: progress=%llu/%llu\n",
                   (unsigned long long)total, (unsigned long long)size);
            while (next_progress <= total)
                next_progress += PROGRESS_STEP;
        }
    }

    printf("STORAGE_READ: PASS bytes=%llu sparse_digest=%llx\n",
           (unsigned long long)total, (unsigned long long)digest);
    free(buf);
    close(fd);
    return 0;
}

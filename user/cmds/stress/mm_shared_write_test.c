/* Two-boot MAP_SHARED dirty-state gate (MM-03), guest side.
 *
 * The host gate (tools/mm-shared-write-gate.py) runs this twice against the
 * same ext4 image: once in "write" mode, then a clean `sync` + `poweroff`, then
 * again in "verify" mode after a cold boot.  The property under test is that a
 * MAP_SHARED store stays dirty until the shutdown path flushes it -- neither an
 * mprotect(PROT_READ) downgrade of a live mapping, nor munmap, nor leaving the
 * mapping live may drop the dirty state so that sync() writes stale or zero
 * bytes.
 *
 * Deliberately no msync() anywhere: calling it would flush the pages by hand
 * and make the gate vacuous.  The only flush is the host's `sync`.
 *
 * The pattern is one pure function shared by write and verify so the two can
 * never drift, and it mixes the page index, the in-page offset and a per-shape
 * seed.  That makes an all-zero readback, a stale page left by another shape,
 * and a page shifted by one all fail, rather than matching by luck.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#define MM_PAGE_SIZE 4096
#define MM_PAGES 6
#define MM_LEN ((size_t)MM_PAGES * MM_PAGE_SIZE)

/* One mapping shape per file, so the host can assert each one actually ran. */
static const struct {
    const char *name;
    unsigned seed;
} SHAPES[] = {
    {"mprotect", 0x13u},
    {"munmap", 0x57u},
    {"live", 0x9Bu},
};
#define SHAPE_COUNT (sizeof(SHAPES) / sizeof(SHAPES[0]))

static unsigned char mm_pattern(unsigned seed, size_t off)
{
    unsigned page = (unsigned)(off / MM_PAGE_SIZE);
    unsigned in_page = (unsigned)(off % MM_PAGE_SIZE);
    return (unsigned char)((seed * 0x2Du) ^ (page * 0x9Du) ^
                           (in_page * 0x1Fu) ^ 0x5Au);
}

static void mm_path(char *buf, size_t len, const char *shape)
{
    snprintf(buf, len, "/extra/mm-shared-write-%s.bin", shape);
}

/* write mode: lay down one shape's bytes, apply its mapping operation, and for
 * the shapes that must be live at shutdown fork a holder that keeps the
 * mapping (and fd) alive while the parent returns to the shell.  The parent
 * prints the marker only after the holder exists, so the host's `sync` cannot
 * run before the mapping is in the state the shape promises. */
static int mm_write(const char *shape, unsigned seed)
{
    char path[128];
    mm_path(path, sizeof(path), shape);

    int fd = open(path, O_CREAT | O_RDWR | O_TRUNC, 0644);
    if (fd < 0) {
        printf("MM_SHARED_WRITE: FAIL %s open errno=%d\n", shape, errno);
        return 1;
    }
    if (ftruncate(fd, (off_t)MM_LEN) < 0) {
        printf("MM_SHARED_WRITE: FAIL %s ftruncate errno=%d\n", shape, errno);
        close(fd);
        return 1;
    }

    unsigned char *m = mmap(NULL, MM_LEN, PROT_READ | PROT_WRITE,
                            MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        printf("MM_SHARED_WRITE: FAIL %s mmap errno=%d\n", shape, errno);
        close(fd);
        return 1;
    }
    for (size_t i = 0; i < MM_LEN; i++)
        m[i] = mm_pattern(seed, i);

    int hold = 0;
    if (strcmp(shape, "munmap") == 0) {
        if (munmap(m, MM_LEN) < 0) {
            printf("MM_SHARED_WRITE: FAIL %s munmap errno=%d\n", shape, errno);
            close(fd);
            return 1;
        }
    } else if (strcmp(shape, "mprotect") == 0) {
        if (mprotect(m, MM_LEN, PROT_READ) < 0) {
            printf("MM_SHARED_WRITE: FAIL %s mprotect errno=%d\n", shape, errno);
            close(fd);
            return 1;
        }
        hold = 1;
    } else {
        hold = 1;
    }

    if (hold) {
        pid_t pid = fork();
        if (pid < 0) {
            printf("MM_SHARED_WRITE: FAIL %s fork errno=%d\n", shape, errno);
            close(fd);
            return 1;
        }
        if (pid == 0) {
            /* Keep the mapping and its fd live until poweroff tears the
             * process down; never touch the data again. */
            for (;;)
                pause();
        }
    }

    printf("MM_SHARED_WRITE: WROTE %s\n", shape);
    fflush(stdout);
    close(fd);
    return 0;
}

static int mm_verify(const char *shape, unsigned seed)
{
    char path[128];
    mm_path(path, sizeof(path), shape);

    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        printf("MM_SHARED_WRITE: FAIL %s missing errno=%d\n", shape, errno);
        return 1;
    }

    struct stat st;
    if (fstat(fd, &st) < 0) {
        printf("MM_SHARED_WRITE: FAIL %s stat errno=%d\n", shape, errno);
        close(fd);
        return 1;
    }
    if (st.st_size != (off_t)MM_LEN) {
        printf("MM_SHARED_WRITE: FAIL %s short size=%lld want=%llu\n", shape,
               (long long)st.st_size, (unsigned long long)MM_LEN);
        close(fd);
        return 1;
    }

    unsigned char buf[MM_LEN];
    size_t got = 0;
    while (got < MM_LEN) {
        ssize_t n = read(fd, buf + got, MM_LEN - got);
        if (n < 0) {
            printf("MM_SHARED_WRITE: FAIL %s read errno=%d\n", shape, errno);
            close(fd);
            return 1;
        }
        if (n == 0)
            break;
        got += (size_t)n;
    }
    close(fd);
    if (got != MM_LEN) {
        printf("MM_SHARED_WRITE: FAIL %s short read=%zu want=%zu\n", shape, got,
               MM_LEN);
        return 1;
    }

    for (size_t i = 0; i < MM_LEN; i++) {
        unsigned char want = mm_pattern(seed, i);
        if (buf[i] != want) {
            printf("MM_SHARED_WRITE: FAIL %s content off=%zu page=%zu "
                   "got=%u want=%u\n",
                   shape, i, i / MM_PAGE_SIZE, (unsigned)buf[i],
                   (unsigned)want);
            return 1;
        }
    }

    printf("MM_SHARED_WRITE: PASS %s\n", shape);
    fflush(stdout);
    return 0;
}

static int mm_usage(void)
{
    printf("usage: mm_shared_write_test write|verify [shape]\n");
    return 2;
}

int main(int argc, char **argv)
{
    if (argc < 2)
        return mm_usage();

    int do_write = strcmp(argv[1], "write") == 0;
    int do_verify = strcmp(argv[1], "verify") == 0;
    if (!do_write && !do_verify)
        return mm_usage();

    int failures = 0;
    int matched = 0;
    for (size_t s = 0; s < SHAPE_COUNT; s++) {
        if (argc >= 3 && strcmp(argv[2], SHAPES[s].name) != 0)
            continue;
        matched++;
        if (do_write)
            failures += mm_write(SHAPES[s].name, SHAPES[s].seed);
        else
            failures += mm_verify(SHAPES[s].name, SHAPES[s].seed);
    }
    if (!matched)
        return mm_usage();

    if (do_verify)
        printf("MM_SHARED_WRITE: VERIFY %s failures=%d\n",
               failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

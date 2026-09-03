/* V8-style virtual memory reservation probe (Node.js bring-up debug).
 *
 * Reproduces the reservation patterns V8 uses at startup on a
 * pointer-compression build, each printed with PASS/FAIL + errno:
 *   P1  4 GiB PROT_NONE anonymous reservation (hint=0)
 *   P2  reservation at a 4 GiB-aligned hint (hint honored?)
 *   P3  over-reserve 8 GiB, then MAP_FIXED 4 GiB at the aligned base
 *   P4  mprotect a 2 MiB subrange RW + write/readback (commit path)
 *   P5  MAP_FIXED_NOREPLACE on the occupied range (must fail EEXIST)
 *   P6  second 4 GiB cage alongside the first (coexistence)
 *   P7  32 GiB PROT_NONE reservation (VA headroom check)
 */
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define GIB (1024ull * 1024 * 1024)

static int failures;

static void report(const char *name, void *got, int expect_ok)
{
    if (got == MAP_FAILED) {
        printf("MMPROBE: %s FAIL errno=%d (%s)\n", name, errno,
               strerror(errno));
        if (expect_ok)
            failures++;
        return;
    }
    printf("MMPROBE: %s ok addr=%p\n", name, got);
    if (!expect_ok)
        failures++;
}

int main(void)
{
    printf("MMPROBE: start\n");

    void *p1 = mmap(NULL, 4 * GIB, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    report("P1 reserve-4G-anywhere", p1, 1);

    void *hint = (void *)(4 * GIB);
    void *p2 = mmap(hint, 4 * GIB, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    report("P2 reserve-4G-aligned-hint", p2, 1);
    if (p2 != MAP_FAILED)
        printf("MMPROBE: P2 hint-honored=%s\n",
               p2 == hint ? "yes" : "NO");

    /* P3: the fallback path -- over-reserve then trim to alignment. */
    if (p1 != MAP_FAILED)
        munmap(p1, 4 * GIB);
    if (p2 != MAP_FAILED)
        munmap(p2, 4 * GIB);
    void *big = mmap(NULL, 8 * GIB, PROT_NONE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    report("P3a over-reserve-8G", big, 1);
    if (big != MAP_FAILED) {
        unsigned long base = ((unsigned long)big + 4 * GIB - 1) &
                             ~(4 * GIB - 1);
        munmap(big, 8 * GIB);
        void *cage = mmap((void *)base, 4 * GIB, PROT_NONE,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                          MAP_FIXED, -1, 0);
        report("P3b fixed-4G-at-aligned-base", cage, 1);

        if (cage != MAP_FAILED) {
            void *sub = (char *)cage + 64 * 1024 * 1024;
            int mr = mprotect(sub, 2 * 1024 * 1024,
                              PROT_READ | PROT_WRITE);
            printf("MMPROBE: P4a mprotect-RW %s errno=%d\n",
                   mr == 0 ? "ok" : "FAIL", errno);
            if (mr != 0)
                failures++;
            if (mr == 0) {
                volatile char *w = sub;
                w[0] = 42;
                w[1024 * 1024] = 43;
                printf("MMPROBE: P4b commit-rw %s\n",
                       (w[0] == 42 && w[1024 * 1024] == 43) ? "ok"
                                                            : "FAIL");
                if (!(w[0] == 42 && w[1024 * 1024] == 43))
                    failures++;
            }
            void *occ = mmap((char *)cage + 128 * 1024 * 1024, 4096,
                             PROT_NONE,
                             MAP_PRIVATE | MAP_ANONYMOUS |
                             MAP_FIXED_NOREPLACE, -1, 0);
            printf("MMPROBE: P5 fixed-noreplace-on-occupied %s "
                   "(errno=%d)\n",
                   occ == MAP_FAILED && errno == EEXIST ? "ok" : "FAIL",
                   errno);
            if (!(occ == MAP_FAILED && errno == EEXIST))
                failures++;
        }
    }

    void *c2 = mmap(NULL, 4 * GIB, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    report("P6 second-4G-cage", c2, 1);

    void *p7 = mmap(NULL, 32 * GIB, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    report("P7 reserve-32G", p7, 1);

    /* P8: Linux hint semantics -- a hint ABOVE USER_VA_LIMIT must be
     * ignored (fall back to a legal address), never fail.  V8's
     * GetRandomMmapAddr generates hints up to 2^46 and depends on this;
     * the old kernel returned -ENOMEM and V8 aborted with a spurious
     * "MemoryChunk allocation failed" OOM at startup. */
    errno = 0;
    void *p8 = mmap((void *)0xee43940000ul, 512 * 1024,
                    PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    report("P8 high-hint-fallback", p8, 1);
    if (p8 != MAP_FAILED) {
        volatile char *w = p8;
        w[0] = 7;
        if (w[0] != 7)
            failures++;
        munmap(p8, 512 * 1024);
    }

    printf("MMPROBE: %s failures=%d\n", failures ? "FAIL" : "PASS",
           failures);
    return failures ? 1 : 0;
}

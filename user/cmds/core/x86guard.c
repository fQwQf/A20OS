#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define TEST_BYTES (8U * 1024U * 1024U)
#define CHUNK_BYTES (64U * 1024U)

#if !defined(__x86_64__)
int main(void)
{
    puts("X86GUARD: SKIP (requires x86_64)");
    return 0;
}
#else
static long raw_syscall3_preserve_xmm0(long nr, long a0, long a1, long a2,
                                      uint64_t after[2])
{
    const uint64_t sentinel[2] = {
        UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)
    };
    register long rax __asm__("rax") = nr;
    __asm__ __volatile__(
        "movdqu %[sentinel], %%xmm0\n\t"
        "syscall\n\t"
        "movdqu %%xmm0, %[after]\n\t"
        : "+a"(rax), [after] "=m"(*(uint64_t (*)[2])after)
        : "D"(a0), "S"(a1), "d"(a2), [sentinel] "m"(sentinel)
        : "rcx", "r11", "memory", "xmm0");
    return rax;
}

static int check_xmm(const char *op, const uint64_t got[2])
{
    if (got[0] == UINT64_C(0x0123456789abcdef) &&
        got[1] == UINT64_C(0xfedcba9876543210))
        return 0;
    fprintf(stderr, "X86GUARD: FAIL %s clobbered (%016llx %016llx)\n",
            op, (unsigned long long)got[0], (unsigned long long)got[1]);
    return 1;
}

static int raw_error(const char *op, long rc)
{
    fprintf(stderr, "X86GUARD: FAIL %s syscall returned %ld\n", op, rc);
    return 2;
}

int main(int argc, char **argv)
{
    static unsigned char buf[CHUNK_BYTES];
    uint64_t after[2];
    const char *path = argc > 1 ? argv[1] : "/x86guard-probe.tmp";
    int fd = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (fd < 0) { perror("open"); return 2; }
    memset(buf, 0x6d, sizeof(buf));

    unsigned long done = 0;
    while (done < TEST_BYTES) {
        long n = raw_syscall3_preserve_xmm0(1, fd, (long)buf,
                                             sizeof(buf), after);
        if (n < 0) return raw_error("write", n);
        if (check_xmm("write", after)) return 1;
        if (n == 0) { fprintf(stderr, "X86GUARD: FAIL zero write\n"); return 2; }
        done += (unsigned long)n;
    }

    long rc = raw_syscall3_preserve_xmm0(74, fd, 0, 0, after); /* fsync */
    if (rc < 0) return raw_error("fsync", rc);
    if (check_xmm("fsync", after)) return 1;
    if (lseek(fd, 0, SEEK_SET) != 0) { perror("lseek"); return 2; }

    done = 0;
    while (done < TEST_BYTES) {
        memset(buf, 0, sizeof(buf));
        long n = raw_syscall3_preserve_xmm0(0, fd, (long)buf,
                                             sizeof(buf), after);
        if (n < 0) return raw_error("read", n);
        if (check_xmm("read", after)) return 1;
        if (n == 0) { fprintf(stderr, "X86GUARD: FAIL short read\n"); return 2; }
        for (long i = 0; i < n; ++i) {
            if (buf[i] != 0x6d) {
                fprintf(stderr, "X86GUARD: FAIL data mismatch at %lu\n",
                        done + (unsigned long)i);
                return 2;
            }
        }
        done += (unsigned long)n;
    }
    close(fd);
    unlink(path);
    puts("X86GUARD: PASS bytes=8388608 ops=write+fsync+read");
    return 0;
}
#endif

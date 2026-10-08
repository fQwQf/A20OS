#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/wait.h>
#include <unistd.h>

#if !defined(__x86_64__)
int main(void)
{
    puts("FORK_FPU: SKIP (requires x86_64)");
    return 0;
}
#else
typedef struct {
    uint64_t xmm0[2];
    uint64_t xmm15[2];
    uint16_t x87_cw;
    uint32_t mxcsr;
} fp_snapshot_t;

static const uint64_t xmm0_sentinel[2] = {
    UINT64_C(0x0123456789abcdef), UINT64_C(0xfedcba9876543210)
};
static const uint64_t xmm15_sentinel[2] = {
    UINT64_C(0x55aa33cc77ee1199), UINT64_C(0x998811ee77cc33aa)
};
static const uint16_t test_x87_cw = 0x077f; /* masked exceptions, round down */
static const uint32_t test_mxcsr = 0x3f80;  /* masked exceptions, round down */

static long raw_fork_with_fp_snapshot(fp_snapshot_t *snapshot)
{
    uint16_t original_cw;
    uint32_t original_mxcsr;
    long result;

    /* Keep the raw fork syscall and all captures in one asm block.  In
     * particular, no libc wrapper may legally clobber caller-saved XMM regs
     * between fork returning in the child and this snapshot. */
    __asm__ __volatile__(
        "fnstcw %[original_cw]\n\t"
        "stmxcsr %[original_mxcsr]\n\t"
        "fldcw %[test_cw]\n\t"
        "ldmxcsr %[test_mxcsr]\n\t"
        "movdqu %[xmm0_sentinel], %%xmm0\n\t"
        "movdqu %[xmm15_sentinel], %%xmm15\n\t"
        "mov $57, %%eax\n\t" /* SYS_fork */
        "syscall\n\t"
        "movdqu %%xmm0, %[xmm0_after]\n\t"
        "movdqu %%xmm15, %[xmm15_after]\n\t"
        "fnstcw %[x87_after]\n\t"
        "stmxcsr %[mxcsr_after]\n\t"
        "fldcw %[original_cw]\n\t"
        "ldmxcsr %[original_mxcsr]\n\t"
        : "=a"(result),
          [xmm0_after] "=m"(snapshot->xmm0),
          [xmm15_after] "=m"(snapshot->xmm15),
          [x87_after] "=m"(snapshot->x87_cw),
          [mxcsr_after] "=m"(snapshot->mxcsr),
          [original_cw] "=m"(original_cw),
          [original_mxcsr] "=m"(original_mxcsr)
        : [xmm0_sentinel] "m"(xmm0_sentinel),
          [xmm15_sentinel] "m"(xmm15_sentinel),
          [test_cw] "m"(test_x87_cw),
          [test_mxcsr] "m"(test_mxcsr)
        : "rcx", "r11", "memory", "xmm0", "xmm15");
    return result;
}

static int snapshot_matches(const fp_snapshot_t *s)
{
    return s->xmm0[0] == xmm0_sentinel[0] &&
           s->xmm0[1] == xmm0_sentinel[1] &&
           s->xmm15[0] == xmm15_sentinel[0] &&
           s->xmm15[1] == xmm15_sentinel[1] &&
           s->x87_cw == test_x87_cw && s->mxcsr == test_mxcsr;
}

static unsigned snapshot_failure_mask(const fp_snapshot_t *s)
{
    unsigned mask = 0;
    if (s->xmm0[0] != xmm0_sentinel[0]) mask |= 1u << 0;
    if (s->xmm0[1] != xmm0_sentinel[1]) mask |= 1u << 1;
    if (s->xmm15[0] != xmm15_sentinel[0]) mask |= 1u << 2;
    if (s->xmm15[1] != xmm15_sentinel[1]) mask |= 1u << 3;
    if (s->x87_cw != test_x87_cw) mask |= 1u << 4;
    if (s->mxcsr != test_mxcsr) mask |= 1u << 5;
    return mask;
}

static void print_snapshot(const char *who, const fp_snapshot_t *s)
{
    fprintf(stderr,
            "FORK_FPU: %s xmm0=%016llx:%016llx xmm15=%016llx:%016llx "
            "x87cw=%04x mxcsr=%08x\n",
            who,
            (unsigned long long)s->xmm0[1], (unsigned long long)s->xmm0[0],
            (unsigned long long)s->xmm15[1], (unsigned long long)s->xmm15[0],
            s->x87_cw, s->mxcsr);
}

int main(void)
{
    fp_snapshot_t snapshot;
    long child = raw_fork_with_fp_snapshot(&snapshot);
    if (child < 0) {
        fprintf(stderr, "FORK_FPU: FAIL fork returned %ld\n", child);
        return 2;
    }

    if (child == 0) {
        _exit((int)snapshot_failure_mask(&snapshot));
    }

    int status = 0;
    if (waitpid((pid_t)child, &status, 0) != (pid_t)child) {
        perror("FORK_FPU: waitpid");
        return 2;
    }
    if (!snapshot_matches(&snapshot)) {
        print_snapshot("parent", &snapshot);
        fprintf(stderr, "FORK_FPU: FAIL parent state changed across fork\n");
        return 1;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "FORK_FPU: FAIL child exit status=0x%x mask=0x%x\n",
                status, WIFEXITED(status) ? WEXITSTATUS(status) : 0xff);
        return 1;
    }
    puts("FORK_FPU: PASS raw-fork xmm0+xmm15+x87cw+mxcsr");
    return 0;
}
#endif

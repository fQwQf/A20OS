#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__arm__)
/*
 * arm32: the kernel and this program are both built hard-float, so d0-d15 are
 * live across every syscall and every preemption.  A trap frame that does not
 * carry them lets two tasks share one physical register file: whichever ran
 * last wins.
 *
 * The protocol is deterministic rather than statistical.  The parent loads its
 * own sentinels, hands the CPU to a child that loads different ones, and only
 * then reads its registers back.  With per-trap save/restore the parent reads
 * its own values; without it, it reads the child's.
 */
typedef struct {
    uint64_t d[16];
    uint32_t fpscr;
} arm_fp_t;

static void arm_fp_load(const uint64_t d[16])
{
    __asm__ volatile("vldmia %0, {d0-d15}" ::"r"(d)
                     : "memory", "d0", "d1", "d2", "d3", "d4", "d5", "d6",
                       "d7", "d8", "d9", "d10", "d11", "d12", "d13", "d14",
                       "d15");
}

static void arm_fp_save(uint64_t d[16], uint32_t *fpscr)
{
    __asm__ volatile("vstmia %0, {d0-d15}" ::"r"(d) : "memory");
    __asm__ volatile("vmrs %0, fpscr" : "=r"(*fpscr));
}

static void arm_fill(uint64_t d[16], uint64_t seed)
{
    for (int i = 0; i < 16; i++)
        d[i] = seed + (uint64_t)i * UINT64_C(0x0123456789abcdef);
}

int main(void)
{
    int go[2], done[2];
    if (pipe(go) != 0 || pipe(done) != 0) {
        puts("FORK_FPU: FAIL pipe");
        return 1;
    }
    uint64_t mine[16];
    arm_fill(mine, UINT64_C(0x1111111111111111));

    pid_t child = fork();
    if (child < 0) {
        puts("FORK_FPU: FAIL fork");
        return 1;
    }
    if (child == 0) {
        close(go[1]);
        close(done[0]);
        char byte;
        if (read(go[0], &byte, 1) != 1)
            _exit(2);
        uint64_t theirs[16];
        arm_fill(theirs, UINT64_C(0x2222222222222222));
        arm_fp_load(theirs);
        /* Stay on the FPU last so an unsaved switch is unambiguous.  A spin,
         * not usleep: arm32's nanosleep does not wake yet, and the probe must
         * measure the FP context, not that bug. */
        for (volatile int spin = 0; spin < 2000000; spin++)
            ;
        uint64_t after[16];
        uint32_t fpscr;
        arm_fp_save(after, &fpscr);
        int ok = memcmp(after, theirs, sizeof(after)) == 0;
        if (write(done[1], &ok, sizeof(ok)) != sizeof(ok))
            _exit(3);
        _exit(ok ? 0 : 4);
    }
    close(go[0]);
    close(done[1]);

    arm_fp_load(mine);
    if (write(go[1], "x", 1) != 1) {
        puts("FORK_FPU: FAIL go");
        return 1;
    }
    /* The syscall and the child's run both happen before this read. */
    int child_ok = 0;
    if (read(done[0], &child_ok, sizeof(child_ok)) != sizeof(child_ok)) {
        puts("FORK_FPU: FAIL done");
        return 1;
    }
    uint64_t after[16];
    uint32_t fpscr;
    arm_fp_save(after, &fpscr);

    int status = 0;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        printf("FORK_FPU: FAIL child status=%d\n", status);
        return 1;
    }
    if (!child_ok) {
        puts("FORK_FPU: FAIL child registers changed under it");
        return 1;
    }
    puts("FORK_FPU: arm32-child-registers PASS");
    if (memcmp(after, mine, sizeof(after)) != 0) {
        printf("FORK_FPU: FAIL parent d0=0x%llx want 0x%llx\n",
               (unsigned long long)after[0], (unsigned long long)mine[0]);
        return 1;
    }
    puts("FORK_FPU: arm32-parent-registers PASS");
    puts("FORK_FPU: PASS");
    return 0;
}
#elif !defined(__x86_64__)
int main(void)
{
    puts("FORK_FPU: SKIP (requires x86_64 or arm32)");
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

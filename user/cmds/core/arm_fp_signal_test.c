/* ARM32 VFP preservation across asynchronous handlers and scheduler returns. */
#include <stdint.h>
#include <stdio.h>
#include <signal.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__arm__)
struct fp_state { uint64_t d[16]; uint32_t fpscr; };
struct handshake { uint32_t armed, hits; };
static struct handshake *active __attribute__((used));
static uint64_t handler_values[16] __attribute__((used, aligned(8)));

/* No compiler-generated VFP callee-save prologue: the kernel's signal return
 * must restore d8-d15 too. sched_yield in the handler also exercises the
 * scheduler's context save while the handler's distinct FP image is live. */
__attribute__((naked)) static void handler(int sig __attribute__((unused)))
{
    __asm__ volatile(
        "ldr r0, =handler_values\n"
        "vldmia r0, {d0-d15}\n"
        "mov r0, #0x00800000\n"
        "vmsr fpscr, r0\n"
        "ldr r0, =active\n"
        "ldr r0, [r0]\n"
        "ldr r1, [r0, #4]\n"
        "add r1, r1, #1\n"
        "dmb ish\n"
        "str r1, [r0, #4]\n"
        "mov r7, #158\n" /* ARM Linux sched_yield */
        "svc #0\n"
        "svc #0\n"
        "bx lr\n");
}

__attribute__((noinline)) static int window(const struct fp_state *mine,
                                            struct fp_state *after,
                                            struct handshake *shared, uint32_t round)
{
    uint32_t spins = 10000000;
    int ok;
    /* Capture before this ordinary C function's callee-save epilogue. There
     * is no C call inside the window that could restore registers for us. */
    __asm__ volatile(
        "mov %[ok], #1\n"
        "vldmia %[mine], {d0-d15}\n"
        "ldr ip, [%[mine], #128]\n"
        "vmsr fpscr, ip\n"
        "dmb ish\n"
        "str %[round], [%[shared]]\n"
        "1: ldr ip, [%[shared], #4]\n"
        "cmp ip, %[round]\n"
        "bhs 2f\n"
        "subs %[spins], %[spins], #1\n"
        "beq 3f\n"
        "tst %[spins], #0xff\n"
        "bne 1b\n"
        "mov r7, #158\n"
        "svc #0\n"
        "b 1b\n"
        "3: \n"
        "mov %[ok], #0\n"
        "2: vstmia %[after], {d0-d15}\n"
        "vmrs ip, fpscr\n"
        "str ip, [%[after], #128]\n"
        : [ok] "=&r" (ok), [spins] "+&r" (spins)
        : [mine] "r" (mine), [after] "r" (after), [shared] "r" (shared), [round] "r" (round)
        : "memory", "cc", "r0", "r7", "r12", "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
          "d8", "d9", "d10", "d11", "d12", "d13", "d14", "d15");
    return ok;
}

int main(void)
{
    enum { ROUNDS = 1000 };
    setvbuf(stdout, NULL, _IONBF, 0);
    active = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (active == MAP_FAILED) return 1;
    for (unsigned i = 0; i < 16; i++) handler_values[i] = UINT64_C(0xdead0000beef0000) + i;
    struct sigaction action = {0};
    action.sa_handler = handler;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) return 1;
    pid_t parent = getpid(), child = fork();
    if (child < 0) return 1;
    if (!child) {
        for (uint32_t round = 1; round <= ROUNDS; round++) {
            while (__atomic_load_n(&active->armed, __ATOMIC_ACQUIRE) < round) sched_yield();
            if (kill(parent, SIGUSR1)) _exit(2);
            while (__atomic_load_n(&active->hits, __ATOMIC_ACQUIRE) < round) sched_yield();
        }
        _exit(0);
    }
    uint32_t round;
    int failed = 0;
    for (round = 1; round <= ROUNDS; round++) {
        struct fp_state mine, after;
        for (unsigned i = 0; i < 16; i++) mine.d[i] = UINT64_C(0x1020304050607080) + ((uint64_t)round << 24) + i;
        mine.fpscr = 0x00400000;
        if (!window(&mine, &after, active, round)) {
            printf("ARM_FP_SIGNAL: FAIL signal timeout round=%u\n", round); failed = 1; break;
        }
        for (unsigned i = 0; i < 16; i++) {
            if (mine.d[i] != after.d[i]) {
                printf("ARM_FP_SIGNAL: FAIL round=%u d%u=%llx want=%llx\n", round, i,
                       (unsigned long long)after.d[i], (unsigned long long)mine.d[i]);
                failed = 1; break;
            }
        }
        if (mine.fpscr != after.fpscr) {
            printf("ARM_FP_SIGNAL: FAIL round=%u fpscr=%x want=%x\n", round, after.fpscr, mine.fpscr); failed = 1;
        }
        if (failed) break;
    }
    if (failed) kill(child, SIGKILL);
    int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    if (!failed && active->hits != ROUNDS) failed = 1;
    printf("ARM_FP_SIGNAL: rounds=%u signals=%u %s\n", round-1, active->hits, failed ? "FAIL" : "PASS");
    munmap(active, 4096);
    return failed;
}
#else
int main(void) { puts("ARM_FP_SIGNAL: SKIP architecture"); return 77; }
#endif

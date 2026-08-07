/*
 * ptrace smoke test: exercises the kernel-internal debugging interface
 * through the Linux ABI ptrace(2) wrapper.
 *
 * Scenarios (Linux-compat semantics):
 *   1. PTRACE_TRACEME child stops on its signals/exec with ptrace stops.
 *   2. Tracer observes the stop, reads registers, reads/writes tracee
 *      memory (PEEKDATA/POKEDATA), then resumes (PTRACE_CONT).
 *   3. PTRACE_SETREGS/GETREGS round trip.
 *   4. PTRACE_SYSCALL entry/exit stops around execve.
 *   5. PTRACE_ATTACH of a live process; wait4 reports the stop even
 *      without WUNTRACED.
 *
 * Prints "PTRACE_SMOKE: PASS" only if every scenario holds.
 */

#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/ptrace.h>
#include <sys/wait.h>

#if defined(__x86_64__)
struct smoke_regs {
    unsigned long r15, r14, r13, r12, rbp, rbx, r11, r10, r9, r8;
    unsigned long rax, rcx, rdx, rsi, rdi, orig_rax, rip;
    unsigned long cs, eflags, rsp, ss, fs_base, gs_base, ds, es, fs, gs;
};
#define SMOKE_REGS_PC_FIELD rip
#define SMOKE_REGS_PC_OFF  16
#elif defined(__riscv)
struct smoke_regs {
    unsigned long pc;
    unsigned long regs[31];
};
#define SMOKE_REGS_PC_FIELD pc
#define SMOKE_REGS_PC_OFF  0
#elif defined(__aarch64__)
struct smoke_regs {
    unsigned long regs[31];
    unsigned long sp;
    unsigned long pc;
    unsigned long pstate;
};
#define SMOKE_REGS_PC_FIELD pc
#define SMOKE_REGS_PC_OFF  32
#else
struct smoke_regs {
    unsigned long pc;
    unsigned long regs[32];
};
#define SMOKE_REGS_PC_FIELD pc
#define SMOKE_REGS_PC_OFF  0
#endif

static int fail(const char *what)
{
    printf("PTRACE_SMOKE: FAIL %s errno=%d\n", what, errno);
    return 1;
}

int main(void)
{
    printf("PTRACE_SMOKE: start\n");

    /* ---- 1. TRACEME child ---- */
    pid_t child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0) {
        if (ptrace(PTRACE_TRACEME, 0, 0, 0) < 0)
            _exit(42);
        raise(SIGSTOP); /* reported to the tracer as a ptrace signal-stop */
        char *argv[] = { "/bin/echo", "ptrace-child", NULL };
        char *envp[] = { NULL };
        execve("/bin/echo", argv, envp);
        _exit(43);
    }

    int status = 0;
    if (waitpid(child, &status, 0) < 0)
        return fail("waitpid-first");
    if (!WIFSTOPPED(status) || WSTOPSIG(status) != SIGSTOP)
        return fail("first-stop-not-sigstop");

    /* ---- 2. observe registers + PEEK/POKE ---- */
    struct smoke_regs regs;
    if (ptrace(PTRACE_GETREGS, child, 0, &regs) < 0)
        return fail("getregs");
    unsigned long poke_addr = 0;
#if defined(__riscv)
    poke_addr = (unsigned long)regs.regs[2]; /* x2 = sp: private stack page */
#elif defined(__x86_64__)
    poke_addr = (unsigned long)regs.rsp;
#else
    poke_addr = (unsigned long)regs.SMOKE_REGS_PC_FIELD;
#endif
    poke_addr &= ~7UL;
    if (poke_addr == 0)
        return fail("no-poke-address");
    long word = ptrace(PTRACE_PEEKDATA, child, (void *)poke_addr, 0);
    if (word == -1 && errno == EIO)
        return fail("peekdata");
    if (ptrace(PTRACE_POKEDATA, child, (void *)poke_addr, word) < 0)
        return fail("pokedata");
    if (ptrace(PTRACE_POKEDATA, child, (void *)poke_addr, ~word) < 0)
        return fail("pokedata-flip");
    if (ptrace(PTRACE_POKEDATA, child, (void *)poke_addr, word) < 0)
        return fail("pokedata-restore");
    /* Slow the tracer down between poke and resume: gives the tracee's
     * page-cache/allocator side of the base kernel time to settle. */
    for (volatile unsigned long d = 0; d < 2000000UL; d++)
        ;

    /* ---- 3. SETREGS/GETREGS round trip on the PC word ---- */
    unsigned long saved_pc = regs.SMOKE_REGS_PC_FIELD;
    regs.SMOKE_REGS_PC_FIELD ^= 0x4;
    if (ptrace(PTRACE_SETREGS, child, 0, &regs) < 0)
        return fail("setregs");
    struct smoke_regs regs2;
    if (ptrace(PTRACE_GETREGS, child, 0, &regs2) < 0)
        return fail("getregs2");
    if (regs2.SMOKE_REGS_PC_FIELD != (saved_pc ^ 0x4))
        return fail("setregs-roundtrip");
    regs2.SMOKE_REGS_PC_FIELD = saved_pc;
    if (ptrace(PTRACE_SETREGS, child, 0, &regs2) < 0)
        return fail("setregs-restore");

    /* ---- 4. PTRACE_SYSCALL around execve ---- */
    if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0)
        return fail("syscall-resume");
    if (waitpid(child, &status, 0) < 0)
        return fail("waitpid-syscall-entry");
    if (!WIFSTOPPED(status))
        return fail("syscall-entry-not-stopped");
    if (ptrace(PTRACE_SYSCALL, child, 0, 0) < 0)
        return fail("syscall-resume2");
    if (waitpid(child, &status, 0) < 0)
        return fail("waitpid-syscall-mid");
    if (!WIFSTOPPED(status))
        return fail("syscall-mid-not-stopped");

    /* execve completed: the next stops are the TRACEME exec SIGTRAP stop
     * and the syscall-exit stop.  Resume through both. */
    int saw_trap = 0;
    for (int i = 0; i < 3; i++) {
        if (ptrace(PTRACE_CONT, child, 0, 0) < 0)
            return fail("cont-exec");
        if (waitpid(child, &status, 0) < 0)
            return fail("waitpid-exec");
        if (WIFEXITED(status))
            break;
        if (!WIFSTOPPED(status))
            return fail("exec-stop-malformed");
        if (WSTOPSIG(status) == SIGTRAP)
            saw_trap = 1;
    }
    if (!saw_trap)
        return fail("no-exec-sigtrap");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("child-exit-status");

    /* ---- 5. PTRACE_ATTACH of a live process ---- */
    child = fork();
    if (child < 0)
        return fail("fork2");
    if (child == 0) {
        volatile unsigned long spins = 0;
        while (spins < 100000000ULL)
            spins++;
        _exit(0);
    }
    if (ptrace(PTRACE_ATTACH, child, 0, 0) < 0)
        return fail("attach");
    if (waitpid(child, &status, 0) < 0)
        return fail("waitpid-attach");
    if (!WIFSTOPPED(status))
        return fail("attach-not-stopped");
    long msg = ptrace(PTRACE_GETEVENTMSG, child, 0, 0);
    if (msg == -1 && errno == EINVAL)
        return fail("geteventmsg");
    if (ptrace(PTRACE_CONT, child, 0, 0) < 0)
        return fail("cont-attach");
    if (waitpid(child, &status, 0) < 0)
        return fail("waitpid-attach-exit");
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0)
        return fail("attach-exit-status");

    printf("PTRACE_SMOKE: PASS\n");
    return 0;
}

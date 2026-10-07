/*
 * hyp_test — end-to-end vcpu slice test (docs/hypervisor/00-design.md S5).
 *
 * Creates a VM, writes a minimal guest into its RAM, runs it, and asserts the
 * guest left through the legacy SBI shutdown call.  The guest's own output
 * ("HYP") is written to the kernel console by the run loop's
 * console_putchar handler, so it appears on the console between this
 * program's lines -- that is the point of the test: the SBI path really ran,
 * not just the syscall return value.
 *
 * With the argument `echo` the guest ALSO does a console round trip: it polls
 * the modelled 16550's LSR for DR, pops RBR, and writes the byte it got back
 * out through SBI console_putchar, three times, before shutting down.  That
 * is the P0 check on the host-keystrokes-reach-the-guest channel
 * (kernel/hyp/hyp_dev.c, hyp_dev_pump_rx): three host bytes in, three host
 * bytes back out, with nothing but the device model in between.  It is behind
 * an argument rather than unconditional because it CONSUMES host input, and
 * the plain `hyp_test` that smoke-hyp-vcpu runs must keep leaving the next
 * shell line alone.
 *
 * The calls go through the Linux ABI bridge (kernel/abi/linux/
 * sys_a20_bridge.c, numbers from kernel/include/core/syscall_nr.h), because a
 * Linux-ABI task has no Native handle table to name a VM with.
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/syscall.h>

/* Mirror of kernel/include/core/syscall_nr.h; hand-copied the way
 * user/cmds/core/envelope_abi.h does, because musl has no header for the
 * A20OS extensions. */
#define SYS_hyp_vm_create    907
#define SYS_hyp_vm_load      908
#define SYS_hyp_vcpu_create  909
#define SYS_hyp_vcpu_run     910
#define SYS_hyp_vm_destroy   911
#define SYS_hyp_vm_status    914

/* hyp_exit_reason_t, kernel/include/hyp/hyp_vcpu.h (frozen contract). */
#define HYP_EXIT_SHUTDOWN 1

/* Mirror of struct hyp_vm_status, kernel/abi/linux/sys_a20_bridge.c.  Same
 * shape as user/cmds/core/hyp/hyp_guest.h's copy; this program does not link
 * that module, so it carries its own. */
struct hyp_test_status {
    uint64_t exit;
    uint64_t scause;
    uint64_t stval;
    uint64_t htval;
    uint64_t marker_seen;
    uint64_t console_bytes;
    uint64_t rx_bytes;      /* v3: host bytes handed to the guest's UART */
};

/* Where the demo guest lives; the stage-2 root only programs root entries
 * [0, 512 GiB), so this is well inside the limit. */
#define GUEST_BASE 0x80000000ULL
#define GUEST_MEM  (2ULL << 20)

/* qemu-virt's 16550, as the guest spells it: THR/RBR at +0, LSR at +5.  The
 * address has to be MATERIALIZED in a register, not implied: `lbu rd, 5(x0)`
 * reads virtual address 5, not 0x10000005 -- a zero base register is an offset
 * from zero, not an absolute address, and an lbu immediate is 12 signed bits
 * so the address cannot be folded into it either.  Getting this wrong does not
 * fault visibly; the guest reads a low address, takes its own stage-1 page
 * fault (no page tables, satp bare) and dies before the round trip starts. */
#define GUEST_UART_BASE_REG 14   /* a4 */
#define GUEST_UART_LSR_OFF  5
#define GUEST_UART_RBR_OFF  0

/*
 * Minimal guest: print "HYP\n" with four legacy SBI console_putchar calls,
 * then shut down.  Every word below was assembled and disassembled with
 * riscv64-unknown-elf-as -march=rv64ima_zicsr (.option norvc, .option
 * norelax); the encodings are spelled out because this array is the test's
 * only input and a wrong word would show up as a guest fault, not as a
 * compile error.
 *
 * RV64I encodings used:
 *   addi rd, x0, imm  = imm[11:0]<<20 | rs2=0<<20 | funct3=0<<12
 *                        | rs1=0<<15 | rd<<7 | opcode 0x13
 *                        a0 = x10 -> rd<<7 = 0x500,  a7 = x17 -> 0x880
 *   ecall             = SYSTEM (opcode 0x73), funct3 = 0, imm = 0 -> 0x73
 */
#define GUEST_LI_A0(ch)   (0x00000013u | ((uint32_t)(ch) << 20) | (10u << 7))
#define GUEST_LI_A7(v)    (0x00000013u | ((uint32_t)(v)  << 20) | (17u << 7))
#define GUEST_ECALL       0x00000073u

/* li a0,'H'  = 0x48<<20 | 0x500 | 0x13 = 0x04800513
 * li a0,'Y'  = 0x59<<20 | 0x500 | 0x13 = 0x05900513
 * li a0,'P'  = 0x50<<20 | 0x500 | 0x13 = 0x05000513
 * li a0,'\n' = 0x0A<<20 | 0x500 | 0x13 = 0x00A00513
 * li a7,1    = 0x001<<20 | 0x880 | 0x13 = 0x00100893   (SBI console_putchar)
 * li a7,8    = 0x008<<20 | 0x880 | 0x13 = 0x00800893   (SBI shutdown)
 *   SBI legacy extension: a7 selects the call, a0 carries the argument. */
static const uint32_t guest_banner[] = {
    GUEST_LI_A0('H'),  GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A0('Y'),  GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A0('P'),  GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A0('\n'), GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A7(8),                    GUEST_ECALL,
};

/*
 * The console round trip, as assembled and disassembled:
 *
 *   0: 10000737  lui  a4, 0x10000       # a4 = 0x10000000, the 16550
 *   4: 00300693  addi a3, zero, 3       # three bytes to echo
 *   8: 00574583  lbu  a1, 5(a4)         # LSR
 *   c: 0015f593  andi a1, a1, 1         # DR alone
 *  10: fe058ce3  beq  a1, zero, 8       # spin while DR is clear
 *  14: 00074603  lbu  a2, 0(a4)         # RBR
 *  18: 00060513  addi a0, a2, 0         # mv a0, a2
 *  1c: 00100893  addi a7, zero, 1
 *  20: 00000073  ecall                 # putchar the byte we just stole
 *  24: fff68693  addi a3, a3, -1
 *  28: fe0690e3  bne  a3, zero, 8       # loop
 *  2c: 00800893  addi a7, zero, 8
 *  30: 00000073  ecall                 # shutdown
 *
 * Two things about this loop are worth stating, because both are properties of
 * the slice rather than of the test:
 *
 *   - IT SPINS IN HARDWARE TRAPS.  Every LSR poll is a second-stage fault, so
 *     a guest waiting for input spins the HOST at trap rate.  That is the
 *     direct cost of a poll-only input channel with no interrupt behind it
 *     (see the v3 note in kernel/include/hyp/hyp_vcpu.h), and it is why the
 *     gate sends the bytes promptly rather than leaving the guest to poll.
 *   - IT CANNOT SEE A BYTE THAT ARRIVES LATE.  Nothing re-checks after the
 *     third echo, so a gate that sends fewer than three bytes hangs here and
 *     is caught by the timeout, not by a wrong answer.  Sending three is the
 *     gate's job, not this loop's.
 */
static const uint32_t guest_echo[] = {
    0x10000737u,   /* lui  a4, 0x10000     -- UART page */
    0x00300693u,   /* addi a3, zero, 3     */
    0x00574583u,   /* lbu  a1, 5(a4)       -- LSR */
    0x0015f593u,   /* andi a1, a1, 1       -- DR */
    0xfe058ce3u,   /* beq  a1, zero, -8    -- spin */
    0x00074603u,   /* lbu  a2, 0(a4)       -- RBR */
    0x00060513u,   /* addi a0, a2, 0       -- mv a0, a2 */
    0x00100893u,   /* addi a7, zero, 1     */
    0x00000073u,   /* ecall                -- putchar */
    0xfff68693u,   /* addi a3, a3, -1      */
    0xfe0690e3u,   /* bne  a3, zero, -32   -- loop */
    0x00800893u,   /* addi a7, zero, 8     */
    0x00000073u,   /* ecall                -- shutdown */
};

/* Bytes the guest is told to expect when it is in echo mode.  Kept next to
 * the encodings so the two cannot drift apart silently. */
#define GUEST_ECHO_N 3

static int fail(const char *what, long rc)
{
    printf("HYP_VCPU_TEST: FAIL %s (rc=%ld errno=%d)\n", what, rc, errno);
    return 1;
}

static void usage(const char *prog)
{
    printf("Usage: %s [echo]\n"
           "  (no args)  guest prints \"HYP\" over SBI, then shuts down.\n"
           "  echo       additionally: the guest polls the modelled UART's\n"
           "             LSR, pops %d bytes from RBR and writes each back\n"
           "             out through SBI -- a host-keystrokes-to-guest\n"
           "             round trip.  It CONSUMES host input, so the caller\n"
           "             must have bytes ready.\n", prog, GUEST_ECHO_N);
}

int main(int argc, char **argv)
{
    int do_echo = 0;
    if (argc > 1) {
        if (strcmp(argv[1], "echo") != 0 || argc > 2) {
            usage(argv[0]);
            return 2;
        }
        do_echo = 1;
    }

    /* In echo mode the guest is ONE program: the banner followed by the round
     * trip and the shutdown, laid at the same base.  Appending the round trip
     * to the banner's own shutdown would never execute it -- that ecall ends
     * the guest -- so the shutdown word is taken from the banner and the loop
     * carries its own. */
    uint32_t code[sizeof(guest_banner) / sizeof(guest_banner[0]) +
                  sizeof(guest_echo) / sizeof(guest_echo[0])];
    size_t n = sizeof(guest_banner) / sizeof(guest_banner[0]);
    memcpy(code, guest_banner, n * sizeof(code[0]));
    /* Drop the banner's trailing shutdown (li a7,8; ecall). */
    n -= 2;
    if (do_echo) {
        /* BYTES, not elements.  memcpy's third argument is a byte count, and
         * passing the element count here is not a compile error, not a runtime
         * error, and not even a wrong answer for the first word: it copies 13
         * bytes, i.e. three whole words plus the first byte of the fourth.  The
         * guest then fetches `andi a1,a1,1` as 0x00000093, which decodes as
         * `addi ra,x0,0`, takes the branch with the stale LSR byte in a1, walks
         * into the zero fill past the copied image and dies on an instruction
         * fetch fault at pc=0 -- a fault four instructions downstream of the
         * line that is actually wrong.  The only cheap detector is to compare
         * the byte count against the element count by eye, which is why the
         * multiplication is spelled out here. */
        memcpy(code + n, guest_echo, sizeof(guest_echo));
        n += sizeof(guest_echo) / sizeof(guest_echo[0]);
    } else {
        code[n++] = 0x00800893u;   /* li a7, 8 */
        code[n++] = 0x00000073u;   /* ecall   */
    }

    long vm = syscall(SYS_hyp_vm_create, GUEST_MEM);
    if (vm < 0)
        return fail("vm_create", vm);

    long r = syscall(SYS_hyp_vm_load, vm, (unsigned long)GUEST_BASE,
                     (unsigned long)code, n * sizeof(code[0]));
    if (r < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        return fail("vm_load", r);
    }

    long vcpu = syscall(SYS_hyp_vcpu_create, vm, (unsigned long)GUEST_BASE);
    if (vcpu < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        return fail("vcpu_create", vcpu);
    }

    /* The gate drives this run by MARKER: the console round trip only
     * progresses once host bytes have been typed, so this line has to reach
     * the terminal before the guest starts spinning on LSR.  Flushed by hand
     * because the host's stdout is not what decides when a guest can begin --
     * the smoke harness reads the terminal, not this process's stdio. */
    if (do_echo) {
        printf("HYP_VCPU_TEST: guest up, send %d bytes\n", GUEST_ECHO_N);
        fflush(stdout);
    }

    long reason = syscall(SYS_hyp_vcpu_run, vcpu);

    /* Read the ingress count BEFORE destroying the VM: the VM owns the ring
     * the counter lives in.  rx_bytes is the number the gate anchors on, and
     * it is a separate claim from the echo itself -- the echo proves the bytes
     * came back out, rx_bytes proves they were counted on the way in. */
    struct hyp_test_status st;
    memset(&st, 0, sizeof(st));
    long sr = syscall(SYS_hyp_vm_status, vm, &st);

    r = syscall(SYS_hyp_vm_destroy, vm);
    if (r < 0)
        return fail("vm_destroy", r);

    if (reason < 0)
        return fail("vcpu_run", reason);
    if (reason != HYP_EXIT_SHUTDOWN) {
        printf("HYP_VCPU_TEST: FAIL exit=%ld want=%d (shutdown)\n",
               reason, HYP_EXIT_SHUTDOWN);
        return 1;
    }
    if (sr < 0)
        return fail("vm_status", sr);

    printf("HYP_VCPU_TEST: exit=%ld rx_bytes=%llu console_bytes=%llu\n",
           reason, (unsigned long long)st.rx_bytes,
           (unsigned long long)st.console_bytes);

    /* Echo mode's own verdict, so the gate fails HERE rather than on a missing
     * anchor if the device model ever stops moving bytes.  This is a LOWER
     * BOUND, not an equality: fewer than GUEST_ECHO_N means the ring drained
     * before the guest got its three bytes.  More is the NORMAL case and is
     * accepted on purpose -- the gate sends the line with a trailing newline,
     * that newline is one more byte for the pump to take, and the guest stops
     * reading the moment it has echoed three.  The passing run therefore
     * reports rx_bytes=GUEST_ECHO_N+1 (4), and nobody should "fix" the extra
     * byte by tightening this to ==.  What a larger value would mean is the
     * guest shutting down with input still queued, and this check cannot tell
     * that apart from the trailing newline; the echo itself (a lone ^HYP$ line
     * in the log) is what pins the count at three. */
    if (do_echo && st.rx_bytes < GUEST_ECHO_N) {
        printf("HYP_VCPU_TEST: FAIL echo got %llu host byte(s), want %d\n",
               (unsigned long long)st.rx_bytes, GUEST_ECHO_N);
        return 1;
    }

    printf("HYP_VCPU_TEST: PASS\n");
    return 0;
}
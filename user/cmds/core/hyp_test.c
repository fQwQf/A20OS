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

/* hyp_exit_reason_t, kernel/include/hyp/hyp_vcpu.h (frozen contract). */
#define HYP_EXIT_SHUTDOWN 1

/* Where the demo guest lives; the stage-2 root only programs root entries
 * [0, 512 GiB), so this is well inside the limit. */
#define GUEST_BASE 0x80000000ULL
#define GUEST_MEM  (2ULL << 20)

/*
 * Minimal guest: print "HYP\n" with four legacy SBI console_putchar calls,
 * then shut down.  Every word below was assembled and disassembled with
 * riscv64-unknown-elf-as -march=rv64ima; the encodings are spelled out
 * because this array is the test's only input and a wrong word would show up
 * as a guest fault, not as a compile error.
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
static const uint32_t guest_code[] = {
    GUEST_LI_A0('H'),  GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A0('Y'),  GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A0('P'),  GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A0('\n'), GUEST_LI_A7(1), GUEST_ECALL,
    GUEST_LI_A7(8),                    GUEST_ECALL,
};

static int fail(const char *what, long rc)
{
    printf("HYP_VCPU_TEST: FAIL %s (rc=%ld errno=%d)\n", what, rc, errno);
    return 1;
}

int main(void)
{
    long vm = syscall(SYS_hyp_vm_create, GUEST_MEM);
    if (vm < 0)
        return fail("vm_create", vm);

    long r = syscall(SYS_hyp_vm_load, vm, (unsigned long)GUEST_BASE,
                     (unsigned long)guest_code, sizeof(guest_code));
    if (r < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        return fail("vm_load", r);
    }

    long vcpu = syscall(SYS_hyp_vcpu_create, vm, (unsigned long)GUEST_BASE);
    if (vcpu < 0) {
        syscall(SYS_hyp_vm_destroy, vm);
        return fail("vcpu_create", vcpu);
    }

    long reason = syscall(SYS_hyp_vcpu_run, vcpu);
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

    printf("HYP_VCPU_TEST: PASS\n");
    return 0;
}
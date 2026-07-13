    .abiversion 2

#include "asm/ppc64-regs.h"

    .section .text
    .globl __trap_from_user
    .globl __return_to_user
    .globl __trap_from_kernel
    .globl user_trap_return

    .macro LOAD_ADDR reg,sym
    lis     \reg,\sym@highest
    ori     \reg,\reg,\sym@higher
    sldi    \reg,\reg,32
    oris    \reg,\reg,\sym@high
    ori     \reg,\reg,\sym@l
    .endm

    .macro LOAD_TRAP_SCRATCH reg
    lis     \reg,0xc000
    sldi    \reg,\reg,32
    ori     \reg,\reg,PPC64_TRAP_SCRATCH_PA
    .endm

__trap_from_user:
    /*
     * The real-mode vector at 0xc00 saved r0-r3 and SRR0/SRR1 in physical
     * scratch memory, then returned here with translation enabled.
     */
    LOAD_TRAP_SCRATCH 12

    ld      1,64(12)
    addi    1,1,-336

    ld      4,104(12)
    std     4,32(1)
    std     5,40(1)
    std     6,48(1)
    std     7,56(1)
    std     8,64(1)
    std     9,72(1)
    std     10,80(1)
    std     11,88(1)
    std     13,104(1)
    std     14,112(1)
    std     15,120(1)
    std     16,128(1)
    std     17,136(1)
    std     18,144(1)
    std     19,152(1)
    std     20,160(1)
    std     21,168(1)
    std     22,176(1)
    std     23,184(1)
    std     24,192(1)
    std     25,200(1)
    std     26,208(1)
    std     27,216(1)
    std     28,224(1)
    std     29,232(1)
    std     30,240(1)
    std     31,248(1)

    ld      4,0(12)
    std     4,0(1)
    ld      4,8(12)
    std     4,8(1)
    ld      4,16(12)
    std     4,16(1)
    ld      4,24(12)
    std     4,24(1)
    ld      4,96(12)
    std     4,96(1)
    ld      4,32(12)
    std     4,256(1)
    ld      4,40(12)
    std     4,264(1)

    ld      4,120(12)
    std     4,272(1)
    ld      4,128(12)
    std     4,280(1)
    ld      4,136(12)
    std     4,288(1)
    ld      4,112(12)
    std     4,296(1)
    std     13,304(1)
    addi    4,1,336
    std     4,312(1)

    ld      13,72(12)
    ld      2,80(12)
    /*
     * ELFv2 callees use the caller's 32-byte linkage area.  Keep that area
     * below the trap frame so a C prologue cannot overwrite saved GPRs.
     */
    mr      31,1
    addi    1,1,-32
    mr      3,31
    bl      ppc64_trap_dispatch
    mr      1,31
    b       __return_to_user

__return_to_user:
    mr      12,1
    ld      4,256(12)
    mtsrr0  4
    ld      4,264(12)
    mtsrr1  4
    ld      4,272(12)
    mtctr   4
    ld      4,280(12)
    mtlr    4
    ld      4,288(12)
    mtxer   4
    ld      4,296(12)
    mtcr    4

    ld      0,0(12)
    ld      2,16(12)
    ld      3,24(12)
    ld      4,32(12)
    ld      5,40(12)
    ld      6,48(12)
    ld      7,56(12)
    ld      8,64(12)
    ld      9,72(12)
    ld      10,80(12)
    ld      11,88(12)
    ld      13,104(12)
    ld      14,112(12)
    ld      15,120(12)
    ld      16,128(12)
    ld      17,136(12)
    ld      18,144(12)
    ld      19,152(12)
    ld      20,160(12)
    ld      21,168(12)
    ld      22,176(12)
    ld      23,184(12)
    ld      24,192(12)
    ld      25,200(12)
    ld      26,208(12)
    ld      27,216(12)
    ld      28,224(12)
    ld      29,232(12)
    ld      30,240(12)
    ld      31,248(12)
    ld      1,8(12)
    ld      12,96(12)
    rfid

__trap_from_kernel:
3:  b 3b

user_trap_return:
    LOAD_TRAP_SCRATCH 12
    /*
     * pseries firmware/hypercalls do not preserve the guest SPRG scratch
     * value installed during early boot.  Exception entry runs in real
     * mode, so refresh SPRG1 with the physical address immediately before
     * every return to userspace.
     */
    li      11,PPC64_TRAP_SCRATCH_PA
    mtsprg  3,11
    lis     11,__trap_from_user@highest
    ori     11,11,__trap_from_user@higher
    sldi    11,11,32
    oris    11,11,__trap_from_user@high
    ori     11,11,__trap_from_user@l
    std     11,56(12)
    ld      11,312(1)
    std     11,64(12)
    ld      11,304(1)
    std     11,72(12)
    std     2,80(12)
    b       __return_to_user

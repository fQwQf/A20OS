    .abiversion 2

#include "asm/ppc64-regs.h"

    .section .text
    .globl __trap_from_user
    .globl __return_to_user
    .globl __trap_from_kernel
    .globl user_trap_return

    .set PPC64_VECTOR_CONTEXT_OFFSET, 336
    .set PPC64_TRAP_CONTEXT_SIZE, 848

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

    .macro SAVE_VECTOR_REG reg
    stvx    \reg,1,4
    addi    4,4,16
    .endm

    .macro LOAD_VECTOR_REG reg
    lvx     \reg,12,4
    addi    4,4,16
    .endm

__trap_from_user:
    /*
     * The real-mode vector at 0xc00 saved r0-r3 and SRR0/SRR1 in physical
     * scratch memory, then returned here with translation enabled.
     *
     * Choose the trap frame location from the interrupted MSR.PR so a
     * kernel-mode exception (e.g. the SLOF console `sc 1` hypercall fired
     * from the middle of a write syscall) gets a frame below the current
     * kernel stack instead of overwriting the outer user trap frame.
     */
    LOAD_TRAP_SCRATCH 12

    ld      4,40(12)
    andi.   4,4,PPC64_MSR_PR
    beq     1f
    mfsprg  1,2
    b       2f
1:
    ld      1,8(12)
2:
    addi    1,1,-PPC64_TRAP_CONTEXT_SIZE

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
    ld      4,72(12)
    std     4,304(1)
    addi    4,1,PPC64_TRAP_CONTEXT_SIZE
    std     4,312(1)

    /* VMX is lazy-enabled, so only used contexts contain live vectors. */
    ld      4,40(12)
    andis.  4,4,0x0200
    beq     .Lvector_save_done
    li      4,PPC64_VECTOR_CONTEXT_OFFSET
    SAVE_VECTOR_REG 0
    SAVE_VECTOR_REG 1
    SAVE_VECTOR_REG 2
    SAVE_VECTOR_REG 3
    SAVE_VECTOR_REG 4
    SAVE_VECTOR_REG 5
    SAVE_VECTOR_REG 6
    SAVE_VECTOR_REG 7
    SAVE_VECTOR_REG 8
    SAVE_VECTOR_REG 9
    SAVE_VECTOR_REG 10
    SAVE_VECTOR_REG 11
    SAVE_VECTOR_REG 12
    SAVE_VECTOR_REG 13
    SAVE_VECTOR_REG 14
    SAVE_VECTOR_REG 15
    SAVE_VECTOR_REG 16
    SAVE_VECTOR_REG 17
    SAVE_VECTOR_REG 18
    SAVE_VECTOR_REG 19
    SAVE_VECTOR_REG 20
    SAVE_VECTOR_REG 21
    SAVE_VECTOR_REG 22
    SAVE_VECTOR_REG 23
    SAVE_VECTOR_REG 24
    SAVE_VECTOR_REG 25
    SAVE_VECTOR_REG 26
    SAVE_VECTOR_REG 27
    SAVE_VECTOR_REG 28
    SAVE_VECTOR_REG 29
    SAVE_VECTOR_REG 30
    SAVE_VECTOR_REG 31
.Lvector_save_done:

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

    ld      4,264(12)
    andis.  4,4,0x0200
    beq     .Lvector_restore_done
    mfmsr   4
    oris    4,4,0x0200
    mtmsrd  4
    isync
    li      4,PPC64_VECTOR_CONTEXT_OFFSET
    LOAD_VECTOR_REG 0
    LOAD_VECTOR_REG 1
    LOAD_VECTOR_REG 2
    LOAD_VECTOR_REG 3
    LOAD_VECTOR_REG 4
    LOAD_VECTOR_REG 5
    LOAD_VECTOR_REG 6
    LOAD_VECTOR_REG 7
    LOAD_VECTOR_REG 8
    LOAD_VECTOR_REG 9
    LOAD_VECTOR_REG 10
    LOAD_VECTOR_REG 11
    LOAD_VECTOR_REG 12
    LOAD_VECTOR_REG 13
    LOAD_VECTOR_REG 14
    LOAD_VECTOR_REG 15
    LOAD_VECTOR_REG 16
    LOAD_VECTOR_REG 17
    LOAD_VECTOR_REG 18
    LOAD_VECTOR_REG 19
    LOAD_VECTOR_REG 20
    LOAD_VECTOR_REG 21
    LOAD_VECTOR_REG 22
    LOAD_VECTOR_REG 23
    LOAD_VECTOR_REG 24
    LOAD_VECTOR_REG 25
    LOAD_VECTOR_REG 26
    LOAD_VECTOR_REG 27
    LOAD_VECTOR_REG 28
    LOAD_VECTOR_REG 29
    LOAD_VECTOR_REG 30
    LOAD_VECTOR_REG 31
.Lvector_restore_done:

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
     * pseries firmware/hypercalls do not preserve the guest SPRG values
     * installed during early boot.  Refresh the real-mode scratch pointer
     * and publish this task's kernel stack before every user return.
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
    mtsprg  2,11
    std     11,64(12)
    ld      11,304(1)
    std     11,72(12)
    std     2,80(12)
    b       __return_to_user

    .globl arch_return_to_user
arch_return_to_user:
    mr      1,3
    b       user_trap_return

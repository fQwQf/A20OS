    .abiversion 2

#include "asm/ppc64-regs.h"

    .set RADIX_VALID,      0x8000000000000000
    .set RADIX_LEAF,       0x4000000000000000
    .set RADIX_REF,        0x0000000000000100
    .set RADIX_CHANGE,     0x0000000000000080
    .set RADIX_PRIV,       0x0000000000000008
    .set RADIX_READ,       0x0000000000000004
    .set RADIX_WRITE,      0x0000000000000002
    .set RADIX_EXEC,       0x0000000000000001
    .set RADIX_DIR_BITS,   9
    .set RADIX_KERNEL_PTE, 0xc00000000000018f

    .macro LOAD_PHYS_ADDR reg,sym
    lis     \reg,\sym@highest
    ori     \reg,\reg,\sym@higher
    sldi    \reg,\reg,32
    oris    \reg,\reg,\sym@high
    ori     \reg,\reg,\sym@l
    add     \reg,\reg,29
    .endm

    .macro LOAD_VIRT_ADDR reg,sym
    lis     \reg,\sym@highest
    ori     \reg,\reg,\sym@higher
    sldi    \reg,\reg,32
    oris    \reg,\reg,\sym@high
    ori     \reg,\reg,\sym@l
    .endm

    .macro LOAD_IMM64 reg,value
    lis     \reg,(\value)@highest
    ori     \reg,\reg,(\value)@higher
    sldi    \reg,\reg,32
    oris    \reg,\reg,(\value)@high
    ori     \reg,\reg,(\value)@l
    .endm

    .section .text.entry
    .globl _start
_start:
    /*
     * SLOF's go-direct entry is 32-bit big-endian.  Install a tiny
     * program-check vector that returns to _start+0x100 in 64-bit LE mode.
     */
    .byte 0x39, 0x40, 0x07, 0x00
    .byte 0x3d, 0x60, 0x7c, 0x7a
    .byte 0x61, 0x6b, 0x02, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x00
    .byte 0x3d, 0x60, 0x38, 0x63
    .byte 0x61, 0x6b, 0x00, 0x88
    .byte 0x91, 0x6a, 0x00, 0x04
    .byte 0x3d, 0x60, 0x7c, 0x7a
    .byte 0x61, 0x6b, 0x03, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x08
    .byte 0x3d, 0x60, 0x7c, 0x80
    .byte 0x61, 0x6b, 0x00, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x0c
    .byte 0x3d, 0x60, 0x60, 0x84
    .byte 0x61, 0x6b, 0x00, 0x01
    .byte 0x91, 0x6a, 0x00, 0x10
    .byte 0x3d, 0x60, 0x7c, 0x9b
    .byte 0x61, 0x6b, 0x03, 0xa6
    .byte 0x91, 0x6a, 0x00, 0x14
    .byte 0x3d, 0x60, 0x4c, 0x00
    .byte 0x61, 0x6b, 0x00, 0x24
    .byte 0x91, 0x6a, 0x00, 0x18
    .byte 0x7c, 0x00, 0x50, 0x6c
    .byte 0x7c, 0x00, 0x04, 0xac
    .byte 0x7c, 0x00, 0x57, 0xac
    .byte 0x39, 0x4a, 0x00, 0x20
    .byte 0x7c, 0x00, 0x50, 0x6c
    .byte 0x7c, 0x00, 0x04, 0xac
    .byte 0x7c, 0x00, 0x57, 0xac
    .byte 0x4c, 0x00, 0x01, 0x2c
    .byte 0x7f, 0xe0, 0x00, 0x08

    .org 0x100
enter_64le:
    bl      1f
1:
    mflr    30
    clrrdi  30,30,20

    LOAD_VIRT_ADDR 6,_start
    subf    29,6,30

    LOAD_PHYS_ADDR 3,_bss_start
    LOAD_PHYS_ADDR 4,_bss_end
    li      0,0
2:
    cmpld   3,4
    bge     3f
    std     0,0(3)
    addi    3,3,8
    b       2b
3:
    li      9,PPC64_TRAP_SCRATCH_PA
    mtsprg  3,9

    li      3,PPC64_H_SET_MODE
    li      4,PPC64_H_SET_MODE_LE
    li      5,PPC64_H_SET_MODE_RESOURCE_LE
    li      6,0
    li      7,0
    sc      1

    li      3,0x200
    LOAD_PHYS_ADDR 4,ppc64_machine_check_vector
    li      5,ppc64_machine_check_vector_end-ppc64_machine_check_vector
    bl      copy_exception_vector
    li      3,0x300
    LOAD_PHYS_ADDR 4,ppc64_dsi_vector
    li      5,ppc64_dsi_vector_end-ppc64_dsi_vector
    bl      copy_exception_vector
    li      3,0x380
    LOAD_PHYS_ADDR 4,ppc64_dseg_vector
    li      5,ppc64_dseg_vector_end-ppc64_dseg_vector
    bl      copy_exception_vector
    li      3,0x400
    LOAD_PHYS_ADDR 4,ppc64_isi_vector
    li      5,ppc64_isi_vector_end-ppc64_isi_vector
    bl      copy_exception_vector
    li      3,0x480
    LOAD_PHYS_ADDR 4,ppc64_isi_vector
    li      5,ppc64_isi_vector_end-ppc64_isi_vector
    bl      copy_exception_vector
    li      3,0x500
    LOAD_PHYS_ADDR 4,ppc64_external_vector
    li      5,ppc64_external_vector_end-ppc64_external_vector
    bl      copy_exception_vector
    li      3,0x700
    LOAD_PHYS_ADDR 4,ppc64_program_vector
    li      5,ppc64_program_vector_end-ppc64_program_vector
    bl      copy_exception_vector
    li      3,0x800
    LOAD_PHYS_ADDR 4,ppc64_fp_unavailable_vector
    li      5,ppc64_fp_unavailable_vector_end-ppc64_fp_unavailable_vector
    bl      copy_exception_vector
    li      3,0x900
    LOAD_PHYS_ADDR 4,ppc64_decrementer_vector
    li      5,ppc64_decrementer_vector_end-ppc64_decrementer_vector
    bl      copy_exception_vector
    li      3,0xc00
    LOAD_PHYS_ADDR 4,ppc64_syscall_vector
    li      5,ppc64_syscall_vector_end-ppc64_syscall_vector
    bl      copy_exception_vector
    li      3,0xe40
    LOAD_PHYS_ADDR 4,ppc64_emu_assist_vector
    li      5,ppc64_emu_assist_vector_end-ppc64_emu_assist_vector
    bl      copy_exception_vector

    LOAD_PHYS_ADDR 9,__boot_dtb_ptr
    li      0,0
    std     0,0(9)

    LOAD_PHYS_ADDR 1,_stack_end

    /* POWER9 4K radix uses 13/9/9/9 index bits.  Use 2 MiB leaves, so
     * the root points to a PUD, which points to the PMD leaf table. */
    LOAD_PHYS_ADDR 14,boot_pgdir
    LOAD_PHYS_ADDR 15,boot_pud
    LOAD_PHYS_ADDR 16,boot_pmd
    LOAD_PHYS_ADDR 21,boot_pgdir

    LOAD_IMM64 19,RADIX_VALID
    ori     20,15,RADIX_DIR_BITS
    or      20,20,19
    stdbrx  20,0,14
    ori     20,16,RADIX_DIR_BITS
    or      20,20,19
    stdbrx  20,0,15

    LOAD_IMM64 22,RADIX_KERNEL_PTE
    li      23,0
    li      24,512
    mtctr   24
4:
    or      25,23,22
    stdbrx  25,0,16
    addi    16,16,8
    addis   23,23,0x20
    bdnz    4b

    /* Register the process table through PAPR. */
    lis     26,0x2
    LOAD_IMM64 27,0x8000000000000000
    /* RTS=52 bits: low RTS nibble in bits 6..8, high bits in 62..63. */
    LOAD_IMM64 28,0x40000000000000a0
    ori     21,21,13
    or      21,21,28
    stdbrx  21,0,26
    std     0,8(26)
    li      22,16
    stdbrx  21,22,26
    std     0,24(26)
    sync

    li      3,PPC64_H_REGISTER_PROC_TBL
    li      4,(PPC64_PROC_TABLE_NEW | PPC64_PROC_TABLE_RADIX)
    mr      5,26
    li      6,0
    li      7,5
    sc      1
    cmpdi   3,PPC64_H_SUCCESS
    bne     radix_failed

    li      3,0
    mtspr   PPC64_SPR_PID,3
    ptesync

    mfmsr   5
    ori     5,5,(PPC64_MSR_DR | PPC64_MSR_IR)
    mtmsrd  5
    isync

    LOAD_VIRT_ADDR 1,_stack_end
    LOAD_VIRT_ADDR 12,kernel_main
    mtctr   12
    bctr

radix_failed:
    b       radix_failed

copy_exception_vector:
    mr      7,3
    mr      8,5
    mtctr   5
5:
    lbz     6,0(4)
    stb     6,0(3)
    addi    3,3,1
    addi    4,4,1
    bdnz    5b
    li      6,0
6:
    dcbst   7,6
    addi    6,6,32
    cmpld   6,8
    blt     6b
    sync
    li      6,0
7:
    icbi    7,6
    addi    6,6,32
    cmpld   6,8
    blt     7b
    sync
    isync
    blr

    .macro PPC64_EXCEPTION_VECTOR name,cause,tval,interrupt=0
\name:
    mtsprg  0,0
    mtsprg  1,1
    mfsprg  1,3
    std     2,16(1)
    std     3,24(1)
    std     4,104(1)
    mfcr    4
    std     4,112(1)
    mfctr   4
    std     4,120(1)
    mflr    4
    std     4,128(1)
    mfxer   4
    std     4,136(1)
    std     12,96(1)
    mfsprg  0,0
    std     0,0(1)
    mfsprg  0,1
    std     0,8(1)
    mfsrr0  0
    std     0,32(1)
    mfsrr1  0
    std     0,40(1)
    li      0,\cause
    .if \interrupt
    li      2,-1
    sldi    2,2,63
    or      0,0,2
    .endif
    std     0,48(1)
    .if \tval
    mfdar   0
    std     0,88(1)
    .else
    li      0,0
    std     0,88(1)
    .endif
    ld      0,56(1)
    mtsrr0  0
    mfsrr1  0
    li      2,1
    sldi    2,2,15
    ori     2,2,PPC64_MSR_PR
    andc    0,0,2
    li      2,1
    sldi    2,2,63
    or      0,0,2
    li      2,1
    sldi    2,2,61
    or      0,0,2
    ori     0,0,(PPC64_MSR_IR | PPC64_MSR_DR | PPC64_MSR_RI | PPC64_MSR_LE)
    mtsrr1  0
    rfid
\name\()_end:
    .endm

    PPC64_EXCEPTION_VECTOR ppc64_machine_check_vector,0x200,0
    PPC64_EXCEPTION_VECTOR ppc64_dsi_vector,0x300,1
    PPC64_EXCEPTION_VECTOR ppc64_dseg_vector,0x380,1
    PPC64_EXCEPTION_VECTOR ppc64_isi_vector,0x301,0
    PPC64_EXCEPTION_VECTOR ppc64_external_vector,0x500,0,1
    PPC64_EXCEPTION_VECTOR ppc64_program_vector,0x700,0
    PPC64_EXCEPTION_VECTOR ppc64_fp_unavailable_vector,0x700,0
    PPC64_EXCEPTION_VECTOR ppc64_decrementer_vector,0x900,0,1
    PPC64_EXCEPTION_VECTOR ppc64_emu_assist_vector,0x700,0

ppc64_syscall_vector:
    mtsprg  0,0
    mtsprg  1,1
    mfsprg  1,3
    std     2,16(1)
    std     3,24(1)
    std     4,104(1)
    mfcr    4
    std     4,112(1)
    mfctr   4
    std     4,120(1)
    mflr    4
    std     4,128(1)
    mfxer   4
    std     4,136(1)
    std     12,96(1)
    mfsprg  0,0
    std     0,0(1)
    mfsprg  0,1
    std     0,8(1)
    mfsrr0  0
    std     0,32(1)
    mfsrr1  0
    std     0,40(1)
    li      0,0xc00
    std     0,48(1)
    li      0,0
    std     0,88(1)
    ld      0,56(1)
    mtsrr0  0
    mfsrr1  0
    li      2,1
    sldi    2,2,15
    ori     2,2,PPC64_MSR_PR
    andc    0,0,2
    li      2,1
    sldi    2,2,63
    or      0,0,2
    li      2,1
    sldi    2,2,61
    or      0,0,2
    ori     0,0,(PPC64_MSR_IR | PPC64_MSR_DR | PPC64_MSR_RI | PPC64_MSR_LE)
    mtsrr1  0
    rfid
ppc64_syscall_vector_end:

    .section .bss
    .align 16
    .globl boot_pgdir
boot_pgdir:
    .space 65536
    .align 12
boot_pud:
    .space 4096
boot_pmd:
    .space 4096

    .globl __boot_dtb_ptr
__boot_dtb_ptr:
    .quad 0

    .section .bss.stack
    .align 12
_stack_bottom:
    .space 4096 * 16
    .globl _stack_end
_stack_end:

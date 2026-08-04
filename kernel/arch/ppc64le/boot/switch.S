    .abiversion 2

    .section .text
    .globl __switch

    .macro LOAD_ADDR reg,sym
    lis     \reg,\sym@highest
    ori     \reg,\reg,\sym@higher
    sldi    \reg,\reg,32
    oris    \reg,\reg,\sym@high
    ori     \reg,\reg,\sym@l
    .endm

__switch:
    /*
     * Allocate the outgoing task_context_t 192 bytes below the current stack
     * pointer.  The ppc64le ELFv2 C frames of the callers (scheduler ->
     * context_switch) place their saved LR at [orig_r1+16], which overlaps
     * the context's r21 slot if the context were saved at the frame base.
     * Saving strictly below the whole C frame keeps the two disjoint.
     */
    addi    1,1,-192
    mflr    9
    std     9,0(1)
    std     2,8(1)
    std     13,16(1)
    std     14,24(1)
    std     15,32(1)
    std     16,40(1)
    std     17,48(1)
    std     18,56(1)
    std     19,64(1)
    std     20,72(1)
    std     21,80(1)
    std     22,88(1)
    std     23,96(1)
    std     24,104(1)
    std     25,112(1)
    std     26,120(1)
    std     27,128(1)
    std     28,136(1)
    std     29,144(1)
    std     30,152(1)
    std     31,160(1)
    addi    9,1,192
    std     9,168(1)
    LOAD_ADDR 9,ppc64_current_addr_space
    ld      9,0(9)
    std     9,176(1)
    cmpdi   13,0
    beq     1f
    std     1,0(13)
1:
    mr      1,3
    ld      9,0(1)
    /*
     * ELFv2 global entries derive their TOC pointer from r12.  A fresh task
     * resumes by branching to a function entry rather than through a normal
     * bl instruction, so provide the entry address explicitly.
     */
    mr      12,9
    ld      2,8(1)
    ld      13,16(1)
    ld      14,24(1)
    ld      15,32(1)
    ld      16,40(1)
    ld      17,48(1)
    ld      18,56(1)
    ld      19,64(1)
    ld      20,72(1)
    ld      21,80(1)
    ld      22,88(1)
    ld      23,96(1)
    ld      24,104(1)
    ld      25,112(1)
    ld      26,120(1)
    ld      27,128(1)
    ld      28,136(1)
    ld      29,144(1)
    ld      30,152(1)
    ld      31,160(1)
    ld      11,176(1)
    cmpdi   11,0
    beq     2f
    mr      3,11
    bl      ppc64_radix_set_process_root
2:
    /*
     * The address-space helper is a normal call and therefore clobbers LR.
     * Restore the switched task's return address only after that call.
     */
    ld      9,0(1)
    mtlr    9
    mr      12,9
    ld      1,168(1)
    /*
     * Stay in the scheduler's privileged MMU context.  The trap frame's MSR
     * is installed later by user_trap_return with rfid; changing MSR here
     * would also apply the stale context-switch value before the trampoline.
     */
    blr

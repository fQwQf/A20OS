#ifndef _A20_EXT_H
#define _A20_EXT_H

/*
 * Native ABI kernel extension points (0x0D00).
 *
 * Kernel extension programs are small VERIFIED bytecode programs attached
 * to kernel extension points (see kernel/include/ext/kep.h for the
 * instruction set).  They can never corrupt kernel state: the verifier
 * rejects backward jumps and any memory access outside the typed context
 * of the extension point.
 */

#include "a20_types.h"
#include "a20_syscall.h"

/* Syscall-filter context layout and verdicts. */
#define A20_KEP_SCF_ARGS  6
#define A20_KEP_SCF_NR    0
#define A20_KEP_SCF_ARG0  1
#define A20_KEP_SCF_ABI   7
#define A20_KEP_SCF_WORDS 8
#define A20_KEP_SCF_ALLOW 0
#define A20_KEP_SCF_DENY  1
#define A20_KEP_SCF_KILL  2

/* KEP instruction encoding (32-bit): op<<28 | rd<<24 | rs<<20 | aux<<16 | imm16 */
#define A20_KEP_INS(op, rd, rs, aux, imm) \
    (((uint32_t)(op) << 28) | ((uint32_t)(rd) << 24) | \
     ((uint32_t)(rs) << 20) | ((uint32_t)(aux) << 16) | ((uint32_t)(imm) & 0xFFFF))
#define A20_KEP_OP_MOVI 0
#define A20_KEP_OP_MOV  1
#define A20_KEP_OP_LDC  2
#define A20_KEP_OP_STC  3
#define A20_KEP_OP_ALU  4
#define A20_KEP_OP_ADDI 5
#define A20_KEP_OP_ANDI 6
#define A20_KEP_OP_ORI  7
#define A20_KEP_OP_XORI 8
#define A20_KEP_OP_SHLI 9
#define A20_KEP_OP_SHRI 10
#define A20_KEP_OP_JMP  11
#define A20_KEP_OP_JCC  12
#define A20_KEP_OP_EXIT 13
#define A20_KEP_CC_EQ   0
#define A20_KEP_CC_NE   1
#define A20_KEP_CC_LT   2
#define A20_KEP_CC_LE   3
#define A20_KEP_CC_GT   4
#define A20_KEP_CC_GE   5
#define A20_KEP_CC_SIGNED 8

static inline a20_status_t a20_ext_prog_load(const uint32_t *insns,
                                              uint32_t len,
                                              a20_handle_t *out)
{
    a20_status_t st = a20_syscall6(A20_SYS_ext_prog_load, (uint64_t)insns,
                                   len, 0, 0, 0, 0);
    if (st >= 0 && out)
        *out = (a20_handle_t)st;
    return st < 0 ? st : A20_OK;
}

static inline a20_status_t a20_ext_prog_attach(a20_handle_t prog,
                                                uint32_t point_id)
{
    return a20_syscall6(A20_SYS_ext_prog_attach, prog, point_id, 0, 0, 0, 0);
}

static inline a20_status_t a20_ext_prog_detach(a20_handle_t prog,
                                                uint32_t point_id)
{
    return a20_syscall6(A20_SYS_ext_prog_detach, prog, point_id, 0, 0, 0, 0);
}

static inline a20_status_t a20_ext_prog_release(a20_handle_t prog)
{
    return a20_syscall6(A20_SYS_ext_prog_release, prog, 0, 0, 0, 0, 0);
}

static inline a20_status_t a20_ext_point_info(uint32_t point_id,
                                               a20_ext_point_info_t *out)
{
    return a20_syscall6(A20_SYS_ext_point_info, point_id, (uint64_t)out,
                        0, 0, 0, 0);
}

#endif /* _A20_EXT_H */

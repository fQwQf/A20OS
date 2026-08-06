#ifndef _A20_EXT_H
#define _A20_EXT_H

/*
 * Native ABI kernel extension points (0x0D00).
 *
 * Kernel extension programs use the standard Linux eBPF instruction
 * encoding (struct bpf_insn layout, 8 bytes per instruction), so the same
 * program bytes load through the Native ABI and the Linux bpf(2) wrapper.
 * The KEP verifier is stricter than Linux eBPF: jumps must be strictly
 * forward (no loops) and memory access is limited to the extension-point
 * context window reached through R1 (see docs/extensions.md).
 */

#include "a20_types.h"
#include "a20_syscall.h"

/* ---- eBPF instruction encoding ---- */

typedef struct a20_bpf_insn {
    uint8_t  code;
    uint8_t  dst_reg;
    uint8_t  src_reg;
    int16_t  off;
    int32_t  imm;
} a20_bpf_insn_t;

#define A20_BPF_CLASS(c) ((c) & 0x07)
#define A20_BPF_LDX_DW(dst, src, off) \
    ((a20_bpf_insn_t){ 0x01 | 0x18 | 0x60, (dst), (src), (off), 0 })
#define A20_BPF_MOV64_IMM(dst, imm) \
    ((a20_bpf_insn_t){ 0x07 | 0xb0 | 0x00, (dst), 0, 0, (imm) })
#define A20_BPF_ALU64_IMM(op, dst, imm) \
    ((a20_bpf_insn_t){ 0x07 | (op) | 0x00, (dst), 0, 0, (imm) })
#define A20_BPF_ALU64_REG(op, dst, src) \
    ((a20_bpf_insn_t){ 0x07 | (op) | 0x08, (dst), (src), 0, 0 })
#define A20_BPF_JMP_IMM(op, dst, off, imm) \
    ((a20_bpf_insn_t){ 0x05 | (op) | 0x00, (dst), 0, (off), (imm) })
#define A20_BPF_JMP_REG(op, dst, src, off) \
    ((a20_bpf_insn_t){ 0x05 | (op) | 0x08, (dst), (src), (off), 0 })
#define A20_BPF_EXIT() \
    ((a20_bpf_insn_t){ 0x05 | 0x90, 0, 0, 0, 0 })

#define A20_BPF_ADD 0x00
#define A20_BPF_SUB 0x10
#define A20_BPF_AND 0x50
#define A20_BPF_OR  0x40
#define A20_BPF_XOR 0xa0
#define A20_BPF_JEQ 0x10
#define A20_BPF_JNE 0x50
#define A20_BPF_JGT 0x20
#define A20_BPF_JGE 0x30
#define A20_BPF_JLT 0xa0
#define A20_BPF_JLE 0xb0
#define A20_BPF_JSET 0x40

/* Syscall-filter context layout and verdicts. */
#define A20_KEP_SCF_ARGS  6
#define A20_KEP_SCF_NR    0
#define A20_KEP_SCF_ARG0  1
#define A20_KEP_SCF_ABI   7
#define A20_KEP_SCF_WORDS 8
#define A20_KEP_SCF_ALLOW 0
#define A20_KEP_SCF_DENY  1
#define A20_KEP_SCF_KILL  2

static inline a20_status_t a20_ext_prog_load(const a20_bpf_insn_t *insns,
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

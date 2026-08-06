#ifndef _EXT_KEP_H
#define _EXT_KEP_H

/*
 * A20OS kernel extension programs (KEP).
 *
 * A dynamic kernel-extension mechanism designed for the hybrid-kernel
 * model, deliberately unlike a loadable-module facility:
 *
 *  - Extensions are small VERIFIED bytecode programs, not native code:
 *    no arbitrary memory access, no pointers, no unbounded loops, so a
 *    misbehaving extension can never corrupt the kernel.
 *  - Programs use the standard Linux eBPF instruction encoding
 *    (struct bpf_insn: code/dst/src/off/imm, registers R0..R10), so both
 *    the Linux bpf(2) ABI and the Native ABI load the same programs.
 *  - An extension runs only against a typed CONTEXT published by a kernel
 *    extension point (e.g. the syscall filter context below); memory
 *    access is limited to that context window, reached through R1 (the
 *    context pointer) with LDX/STX only.
 *  - Extension points are registered by kernel subsystems; userland loads
 *    programs, attaches them to points, and can detach or close them at
 *    any time.
 *
 * Safety model (verifier, kep.c):
 *  - Every jump must be strictly forward -> structural termination.
 *  - LDX/STX memory access is allowed only through R1 with a constant
 *    non-negative offset within KEP_MAX_CONTEXT_BYTES.
 *  - Programs must end with EXIT; instruction count is bounded.
 */

#include "core/types.h"
#include "core/lock.h"

/*
 * Standard Linux eBPF instruction encoding (struct bpf_insn layout).
 * KEP uses this encoding for ABI compatibility with the Linux bpf(2)
 * wrapper; the verifier and memory model remain KEP's own (forward-only
 * jumps, context-window memory through R1).
 */
typedef struct kep_insn {
    uint8_t  code;    /* opcode: BPF_CLASS/SIZE/MODE/OP/SRC */
    uint8_t  dst_reg;
    uint8_t  src_reg;
    int16_t  off;
    int32_t  imm;
} kep_insn_t;

typedef kep_insn_t bpf_insn_t; /* Linux ABI name for the same layout */

#define KEP_REGS            11   /* R0..R10, R10 is the frame pointer */
#define KEP_MAX_PROG_INSNS  256
#define KEP_MAX_CONTEXT_WORDS 64
#define KEP_MAX_CONTEXT_BYTES (KEP_MAX_CONTEXT_WORDS * 8)

/* Linux eBPF opcode constants used by the verifier/interpreter. */
#define BPF_LD    0x00
#define BPF_LDX   0x01
#define BPF_ST    0x02
#define BPF_STX   0x03
#define BPF_ALU   0x04
#define BPF_JMP   0x05
#define BPF_JMP32 0x06
#define BPF_ALU64 0x07
#define BPF_CLASS(code) ((code) & 0x07)
#define BPF_SIZE(code)  ((code) & 0x18)
#define BPF_MODE(code)  ((code) & 0xe0)
#define BPF_OP(code)    ((code) & 0xf0)
#define BPF_SRC(code)   ((code) & 0x08)
#define BPF_W   0x00
#define BPF_H   0x08
#define BPF_B   0x10
#define BPF_DW  0x18
#define BPF_IMM 0x00
#define BPF_ABS 0x20
#define BPF_IND 0x40
#define BPF_MEM 0x60
#define BPF_K   0x00
#define BPF_X   0x08

#define BPF_ADD  0x00
#define BPF_SUB  0x10
#define BPF_MUL  0x20
#define BPF_DIV  0x30
#define BPF_OR   0x40
#define BPF_AND  0x50
#define BPF_LSH  0x60
#define BPF_RSH  0x70
#define BPF_NEG  0x80
#define BPF_MOD  0x90
#define BPF_XOR  0xa0
#define BPF_MOV  0xb0
#define BPF_ARSH 0xc0

#define BPF_JA    0x00
#define BPF_JEQ   0x10
#define BPF_JGT   0x20
#define BPF_JGE   0x30
#define BPF_JSET  0x40
#define BPF_JNE   0x50
#define BPF_JSGT  0x60
#define BPF_JSGE  0x70
#define BPF_CALL  0x80
#define BPF_EXIT  0x90
#define BPF_JLT   0xa0
#define BPF_JLE   0xb0
#define BPF_JSLT  0xc0
#define BPF_JSLE  0xd0

#define BPF_REG_CTX 1  /* R1 carries the extension-point context pointer */

typedef struct kep_prog kep_prog_t;

typedef struct kep_ctx {
    uint64_t *words;      /* typed window published by the extension point */
    uint32_t nwords;
} kep_ctx_t;

typedef struct kep_attached {
    kep_prog_t *prog;
    struct kep_attached *next;
} kep_attached_t;

typedef struct kep_point {
    uint32_t id;
    const char *name;
    uint32_t nwords;              /* context size in 64-bit words */
    spinlock_t lock;
    kep_attached_t *attached;
} kep_point_t;

/* Extension point ids (the first one is the syscall filter). */
#define KEP_POINT_SYSCALL_FILTER 1

/* Syscall-filter context layout (words). */
#define KEP_SCF_ARGS   6
#define KEP_SCF_NR     0
#define KEP_SCF_ARG0   1
#define KEP_SCF_ARG1   2
#define KEP_SCF_ARG2   3
#define KEP_SCF_ARG3   4
#define KEP_SCF_ARG4   5
#define KEP_SCF_ARG5   6
#define KEP_SCF_ABI    7   /* 0 = Linux ABI, 1 = Native ABI */
#define KEP_SCF_WORDS  8

/* Syscall-filter verdicts returned by the attached program in R0. */
#define KEP_SCF_ALLOW  0
#define KEP_SCF_DENY   1   /* syscall fails with -EACCES */
#define KEP_SCF_KILL   2   /* calling task is terminated */

/* Syscall-filter entry point (kernel/ext/kep_syscall_filter.c). */
void kep_syscall_filter_init(void);
int kep_syscall_filter_check(uint64_t nr, const uint64_t *args, int abi);

/* Register the extension point; called by the owning subsystem at init. */
int kep_register_point(kep_point_t *pt);

/* Load and verify a program (insns = user-space array of bpf_insn_t,
 * len = number of instructions).  Returns a kernel-owned program id (>= 1)
 * or a negative errno.  The program is owned by the calling task; it is
 * released by kep_prog_release() or automatically when the owner exits. */
int kep_prog_load(const bpf_insn_t *insns, uint32_t len);

/* Attach a loaded program to an extension point. */
int kep_prog_attach(int prog_id, uint32_t point_id);

/* Detach from one point (or all points when point_id == 0). */
int kep_prog_detach(int prog_id, uint32_t point_id);

/* Release the program (owner only). */
int kep_prog_release(int prog_id);

/* Linux ABI fd mapping: a loaded program may be bound to a process-local
 * fd (BPF_PROG_LOAD returns a memfd).  kep_prog_find_by_fd() resolves the
 * fd back to the program id with an ownership check. */
int kep_prog_set_fd(int prog_id, int fd);
int kep_prog_find_by_fd(int fd);
void kep_sweep_fds(void);

/* Drop every program owned by `pid` (task-exit path). */
void kep_release_process(int pid);

/* Run the programs attached to `pt` against `ctx`; the first non-ALLOW
 * verdict wins (programs run in attach order). */
uint32_t kep_point_run(kep_point_t *pt, kep_ctx_t *ctx);

/* Query an extension point by id: fill name (bounded) and context size.
 * Returns 0 or -ENOENT. */
int kep_point_query(uint32_t point_id, char *name, size_t name_len,
                    uint32_t *nwords_out);

#endif /* _EXT_KEP_H */

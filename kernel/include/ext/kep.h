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
 *  - An extension runs only against a typed CONTEXT published by a kernel
 *    extension point (e.g. the syscall filter context below); direct
 *    memory access is limited to that context window.
 *  - Extension points are registered by kernel subsystems; userland loads
 *    programs (ext_prog_load), attaches them to points, and can detach or
 *    close them at any time.
 *
 * Instruction set (fixed 32-bit, see kep.c for encodings):
 *   8 x 64-bit registers (R0..R7), R0 is the return value.
 *   MOVI/MOV, LDC (load context word), STC (store context word),
 *   ALU/ALUI (add/sub/and/or/xor/shl/shr), JMP (forward only), Jcc, EXIT.
 *
 * Termination is guaranteed structurally: every jump is strictly forward,
 * verified by a linear scan before the program is accepted.
 */

#include "core/types.h"
#include "core/lock.h"

#define KEP_REGS           8
#define KEP_MAX_PROG_INSNS 256
#define KEP_MAX_CONTEXT_WORDS 64

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

/* Syscall-filter entry point (kernel/ext/kep_syscall_filter.c). */
void kep_syscall_filter_init(void);
int kep_syscall_filter_check(uint64_t nr, const uint64_t *args, int abi);

/* Syscall-filter verdicts returned by the attached program in R0. */
#define KEP_SCF_ALLOW  0
#define KEP_SCF_DENY   1   /* syscall fails with -EACCES */
#define KEP_SCF_KILL   2   /* calling task is terminated */

/* Register the extension point; called by the owning subsystem at init. */
int kep_register_point(kep_point_t *pt);

/* Load and verify a program (instr = user-space array of uint32_t,
 * len = number of instructions).  Returns a kernel-owned program id (>= 1)
 * or a negative errno.  The program is owned by the calling task; it is
 * released by kep_prog_release() or automatically when the owner exits. */
int kep_prog_load(const uint32_t *instr, uint32_t len);

/* Attach a loaded program to an extension point. */
int kep_prog_attach(int prog_id, uint32_t point_id);

/* Detach from one point (or all points when point_id == 0). */
int kep_prog_detach(int prog_id, uint32_t point_id);

/* Release the program (owner only). */
int kep_prog_release(int prog_id);

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

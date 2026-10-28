#ifndef A20_PROC_CKPT_IMAGE_H
#define A20_PROC_CKPT_IMAGE_H

/*
 * C/Rust ABI for kernel/proc/ckpt_image.rs: image-format validation and the
 * restore trap-frame assembly.  The frame model in Rust treats rv64 slot
 * x[0] as the address-space-token scratch rather than a GPR, which is what
 * makes the token-clobber ordering bug unrepresentable there.
 */

#include "core/types.h"
#include "proc/debug_regs.h"

/* Validate an A20CKPT1 header; returns 0 or a negative errno. */
int a20_ckpt_validate_header(uint64_t magic, uint32_t version, uint32_t arch,
                             uint64_t regset_size, uint32_t nr_vma,
                             uint32_t nr_fd, uint32_t expect_arch);

/* Validate one VMA record; returns 0 or a negative errno. */
int a20_ckpt_validate_vma(uint64_t start, uint64_t end, uint64_t vm_flags,
                          uint64_t data_offset, uint64_t file_size,
                          uint64_t user_va_limit, uint64_t page_size);

/* ptrace-facing export/import: export reports hardware semantics (x[0] == 0,
 * never the kernel's address-space token); import writes ONLY the
 * architectural register file and never the address-space-token slot. */
void a20_regs_frame_export_rv64(const void *ctx, proc_debug_regs_t *out);
void a20_regs_frame_import_rv64(void *ctx, const proc_debug_regs_t *regs);

/* The RestoreFrame layout mirror is asserted against trap_context_t at the
 * rv64 call site in kernel/proc/ckpt_restore.c, where the platform headers
 * are already in scope. */


_Static_assert(sizeof(proc_debug_regs_t) == 808, "debug regs size drift");
_Static_assert(__builtin_offsetof(proc_debug_regs_t, pc) == 32 * 8,
               "debug regs pc drift");
_Static_assert(__builtin_offsetof(proc_debug_regs_t, sp) == 33 * 8,
               "debug regs sp drift");
_Static_assert(__builtin_offsetof(proc_debug_regs_t, status) == 34 * 8,
               "debug regs status drift");
_Static_assert(__builtin_offsetof(proc_debug_regs_t, orig_syscall) == 35 * 8,
               "debug regs orig_syscall drift");
_Static_assert(__builtin_offsetof(proc_debug_regs_t, fp) == 36 * 8,
               "debug regs fp drift");
_Static_assert(__builtin_offsetof(proc_debug_regs_t, fcsr) == 100 * 8,
               "debug regs fcsr drift");

#endif /* A20_PROC_CKPT_IMAGE_H */

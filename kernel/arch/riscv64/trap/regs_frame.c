/*
 * Layout contract between the Rust RestoreFrame model
 * (kernel/proc/ckpt_image.rs) and this architecture's trap_context_t.
 * The model treats frame slot x[0] as the address-space-token scratch and
 * the remaining 31 GPRs as a separate array; these asserts keep the two
 * layouts pinned to each other.  Arch-local by construction, so the arch
 * boundary rule for generic kernel code does not apply here.
 */

#include "platform.h"
#include "trap_frame.h"

_Static_assert(sizeof(trap_context_t) == 560, "rv64 trap frame size drift");
_Static_assert(__builtin_offsetof(trap_context_t, x) == 0, "frame gprs drift");
_Static_assert(__builtin_offsetof(trap_context_t, sstatus) == 32 * 8,
               "frame sstatus drift");
_Static_assert(__builtin_offsetof(trap_context_t, sepc) == 33 * 8,
               "frame sepc drift");
_Static_assert(__builtin_offsetof(trap_context_t, kernel_tp) == 35 * 8,
               "frame kernel_tp drift");
_Static_assert(__builtin_offsetof(trap_context_t, f) == 36 * 8,
               "frame fp drift");
_Static_assert(__builtin_offsetof(trap_context_t, fcsr) == 68 * 8,
               "frame fcsr drift");
_Static_assert(__builtin_offsetof(trap_context_t, reserved) == 69 * 8,
               "frame reserved drift");

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

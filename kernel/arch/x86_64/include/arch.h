#ifndef _ARCH_X86_64_H
#define _ARCH_X86_64_H

#define ARCH_HAS_VDSO 1
/* CET shadow stack (map_shadow_stack(2)).  Architectures without it keep
 * Linux's arch-correct -ENOSYS from the shared syscall body. */
#define ARCH_HAS_SHADOW_STACK 1

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

/* x86_64 does not save user FP state on syscall entry.  Fork must therefore
 * snapshot the live state while still in the syscall before the child is
 * first scheduled; copying the parent's saved task context could be stale. */
#define ARCH_TASK_CONTEXT_COPY_USER_FP(ctx) \
    __asm__ __volatile__("fxsave64 (%0)" \
                         : : "r"((ctx)->fpu) : "memory")


/* direct exec leaves lose their text PTE under parallel
 * loader lifetimes, which shows up as dynamic-loader SIGSEGVs. */
#define ARCH_EXE_LEAF_RETAIN_UNSAFE 1

/* the debug registers are available and cheap enough to trap on. */
#define ARCH_HW_SINGLE_STEP 1

/* the kernel's epoll_event really is packed; musl's is not. */
#define ARCH_LINUX_EPOLL_EVENT_PACKED 1

#endif

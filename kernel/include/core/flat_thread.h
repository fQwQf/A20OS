#ifndef A20_CORE_FLAT_THREAD_H
#define A20_CORE_FLAT_THREAD_H
#include "core/types.h"
struct task_t;

/* Permission bits for one hardware-protection region. */
#define ARCH_FLAT_PERM_R 0x1U
#define ARCH_FLAT_PERM_W 0x2U
#define ARCH_FLAT_PERM_X 0x4U

/* One address range the calling flat task may touch from user mode.  The
 * kernel never consults these for its own accesses: on RISC-V PMP applies to
 * U-mode only, so the list is an isolation boundary, not a kernel limit. */
typedef struct arch_flat_region {
    uintptr_t base;
    size_t size;
    uint32_t perms;
} arch_flat_region_t;

/* ISA adapter for the common flat Native thread lifecycle. Attach owns a
 * shared image reference and user-stack pin until final task release. It must
 * not publish the child; errors leave the child safe to destroy. */
void arch_flat_program_enter(void);
/* Lowest address of the calling flat task's user stack, or 0 if none. */
uintptr_t arch_flat_current_stack_base(void);
/* Snapshot the calling flat task's allowed ranges.  Returns the count, 0 when
 * the task owns no flat program, or -ENOSPC when more than `max` ranges exist
 * (the caller then falls back to arch_flat_current_span). */
int arch_flat_region_snapshot(arch_flat_region_t *out, int max);
/* Bounding span of every allowed range; returns size or 0. */
size_t arch_flat_current_span(uintptr_t *base);
int arch_flat_thread_attach(struct task_t *parent, struct task_t *child,
                            uintptr_t entry, uintptr_t arg, uintptr_t top,
                            size_t size, uintptr_t tls);
#endif

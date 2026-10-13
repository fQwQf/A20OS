#ifndef A20_CORE_FLAT_THREAD_H
#define A20_CORE_FLAT_THREAD_H
#include "core/types.h"
struct task_t;
/* ISA adapter for the common flat Native thread lifecycle. Attach owns a
 * shared image reference and user-stack pin until final task release. It must
 * not publish the child; errors leave the child safe to destroy. */
void arch_flat_program_enter(void);
int arch_flat_thread_attach(struct task_t *parent, struct task_t *child,
                            uintptr_t entry, uintptr_t arg, uintptr_t top,
                            size_t size, uintptr_t tls);
#endif

#ifndef A20_CORE_FLAT_PROGRAM_H
#define A20_CORE_FLAT_PROGRAM_H
#include "core/types.h"
struct task_t;
/* ISA adapter only: validate/relocate an image and prepare an unpublished task.
 * Success returns one held task reference and its owned initial stack. Common
 * Native spawn code owns argument/handle setup, publication and rollback. */
typedef struct a20_flat_program {
    struct task_t *task;
    uintptr_t stack_base;
    size_t stack_size;
} a20_flat_program_t;
int arch_flat_program_create(const void *image, size_t length,
                             size_t stack_size, a20_flat_program_t *out);
void arch_flat_program_set_stack(struct task_t *task, uintptr_t startup);
#endif

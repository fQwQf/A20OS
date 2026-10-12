#ifndef A20_ARMV7M_PROGRAM_H
#define A20_ARMV7M_PROGRAM_H
#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "core/types.h"
#include "trap_frame.h"

typedef struct armv7m_program {
    trap_context_t registers;
    void *allocation, *stack;
    uintptr_t image;
    size_t image_size, stack_size;
    void *startup;
    unsigned in_kernel, counted;
} armv7m_program_t;

/* Initial static-PIE profile: no interpreter, TLS or dynamic dependencies.
 * Ownership transfers to the deferred task before publication. */
int mcu_program_spawn(const void *elf, size_t length, size_t stack_size,
                      const void *startup, size_t startup_size,
                      uint32_t affinity);
int mcu_program_user_range(const void *pointer, size_t length);
uint32_t *armv7m_program_svc(uint32_t *frame, const uint32_t *callee,
                             uint32_t exception_return);
extern unsigned mcu_program_live;
#endif
#endif

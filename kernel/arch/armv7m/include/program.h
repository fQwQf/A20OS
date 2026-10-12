#ifndef A20_ARMV7M_PROGRAM_H
#define A20_ARMV7M_PROGRAM_H
#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "core/types.h"
#include "trap_frame.h"
#include "mm/flat_user.h"

#include "core/refcount.h"
struct task_t;
typedef struct armv7m_program_image {
    refcount_t refs;
    a20_flat_space_t memory;
    a20_flat_region_t image_region, stack_region;
    void *allocation, *stack;
    uintptr_t image;
    size_t image_size;
    uintptr_t executable_base[16];
    size_t executable_size[16];
    unsigned executable_count;
} armv7m_program_image_t;
typedef struct armv7m_program {
    trap_context_t registers;
    armv7m_program_image_t *owner;
    a20_flat_region_t *stack_pin;
    void *stack, *startup;
    size_t stack_size;
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
void armv7m_program_tick(uint32_t *frame, const uint32_t *callee,
                         uint32_t exception_return);
extern unsigned mcu_program_live;
#endif
#endif

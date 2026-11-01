#ifndef A20_RISCV32_PROGRAM_H
#define A20_RISCV32_PROGRAM_H
#ifdef CONFIG_MCU_NATIVE_PROGRAM
#include "core/types.h"
#include "core/refcount.h"
#include "mm/flat_user.h"

struct task_t;
struct riscv_mcu_program;

/* One loaded static-PIE image, shared by every thread of the program. */
typedef struct riscv_mcu_program_image {
    refcount_t refs;
    a20_flat_space_t memory;
    a20_flat_region_t image_region, stack_region;
    void *allocation, *stack;
    uintptr_t image;
    size_t image_size;
    uintptr_t executable_base[8];
    size_t executable_size[8];
    unsigned executable_count;
} riscv_mcu_program_image_t;

/* Per-thread view: initial U-mode registers plus the owned stack pin. */
typedef struct riscv_mcu_program {
    riscv_mcu_program_image_t *owner;
    a20_flat_region_t *stack_pin;
    void *stack, *startup;
    size_t stack_size;
    uintptr_t entry;
    uintptr_t arg;
    uintptr_t sp;
    unsigned counted;
} riscv_mcu_program_t;

/* Prepare and publish one trusted flat program.  `startup` is copied to the
 * top of the user stack and its address is passed in a0, matching the Native
 * startup protocol. */
int mcu_program_spawn(const void *elf, size_t length, size_t stack_size,
                      const void *startup, size_t startup_size,
                      uint32_t affinity);
int mcu_program_user_range(const void *pointer, size_t length);
void riscv_mcu_program_release(struct riscv_mcu_program *p);
extern unsigned mcu_program_live;
#endif
#endif

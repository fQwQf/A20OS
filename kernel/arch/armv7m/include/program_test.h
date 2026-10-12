#ifndef A20_MCU_PROGRAM_TEST_H
#define A20_MCU_PROGRAM_TEST_H
#include <stdint.h>
typedef struct mcu_program_test_record {
    volatile uint32_t phase, error;
    uint32_t cpu_before, cpu_after, stack_before, stack_after;
    uint32_t futex_word, futex_checks, futex_waits;
    uint32_t heap_base, heap_checks, allocator_passed;
    uint32_t domain_mask_before, domain_mask_after;
    uint32_t threads_passed, thread_parent_cpu, thread_worker_cpu;
    volatile uint32_t parent_exited, orphan_done;
    uint32_t objects_passed;
    uint32_t balance_mask, balance_jobs, balance_sum;
    uint32_t files_passed, spawn_passed, control_passed;
} mcu_program_test_record_t;
#endif

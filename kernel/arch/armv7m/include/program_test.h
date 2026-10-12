#ifndef A20_MCU_PROGRAM_TEST_H
#define A20_MCU_PROGRAM_TEST_H
#include <stdint.h>
typedef struct mcu_program_test_record {
    volatile uint32_t phase, error;
    uint32_t cpu_before, cpu_after, stack_before, stack_after;
} mcu_program_test_record_t;
#endif

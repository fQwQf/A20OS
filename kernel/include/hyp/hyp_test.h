#ifndef A20_HYP_TEST_H
#define A20_HYP_TEST_H
#include "core/types.h"
/* Architecture-owned guest bytecode. Core test logic contains no ISA tests. */
typedef struct hyp_test_payload {
    const void *code;
    size_t code_size;
    const void *spin;
    size_t spin_size;
    uint64_t check_offset;
    uint32_t check_value;
} hyp_test_payload_t;
const hyp_test_payload_t *hyp_arch_test_payload(void);
#endif

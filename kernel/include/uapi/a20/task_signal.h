/* Optional Native checkpoint operations. Zero flags preserve the original ABI.
 * All masks encode signal N at bit N; bit zero is unused. */
#ifndef A20_UAPI_TASK_SIGNAL_H
#define A20_UAPI_TASK_SIGNAL_H
#include <stdint.h>

#define A20_SIGNAL_THREAD 1ULL
#define A20_SIGNAL_PEEK 2ULL
#define A20_SIGNAL_SELECT 4ULL
#define A20_SIGNAL_MASK_QUERY 2ULL
/* Structured signal_check form: full masks/results on 32- and 64-bit ISAs. */
#define A20_SIGNAL_STRUCT 8ULL
#define A20_SIGNAL_CONTROL_CHECK 0u
#define A20_SIGNAL_CONTROL_MASK 1u
typedef struct a20_signal_control {
    uint32_t size;
    uint32_t version;
    uint32_t operation;
    uint32_t flags;
    uint64_t selected;
    uint64_t mask;
    uint64_t result;
} a20_signal_control_t;
#endif

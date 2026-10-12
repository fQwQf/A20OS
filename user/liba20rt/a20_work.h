#ifndef A20_SDK_WORK_H
#define A20_SDK_WORK_H
#include "a20_sync.h"

/* A bounded shared-memory work range, [begin, end). Initialize before publishing
 * to workers; do not reinitialize or change end until all workers have joined.
 * Each successful take owns one index. Exhaustion is WOULD_BLOCK, not a wait.
 * Uses the common kernel atomic operation, including on flat MCU platforms. */
typedef struct {
    uint32_t next;
    uint32_t end;
} a20_work_range_t;

static inline a20_status_t a20_work_range_init(a20_work_range_t *range,
                                               uint32_t begin, uint32_t end) {
    if (!range || begin > end)
        return -A20_ERR_INVALID_ARGUMENT;
    a20_status_t status = a20_atomic32_store(&range->next, begin);
    if (status == A20_OK)
        range->end = end;
    return status;
}

/* Output is unchanged on exhaustion or syscall failure. A claimed index is not
 * automatically retried if its worker exits: application completion is separate
 * from assignment. This range must belong to one shared address space. */
static inline a20_status_t a20_work_range_take(a20_work_range_t *range,
                                               uint32_t *index) {
    if (!range || !index)
        return -A20_ERR_INVALID_ARGUMENT;
    uint32_t next;
    a20_status_t status = a20_atomic32_load(&range->next, &next);
    if (status != A20_OK)
        return status;
    for (;;) {
        if (next >= range->end)
            return -A20_ERR_WOULD_BLOCK;
        uint32_t observed, exchanged;
        status = a20_atomic32(&range->next, A20_ATOMIC32_COMPARE_EXCHANGE, next,
                              next + 1, &observed, &exchanged);
        if (status != A20_OK)
            return status;
        if (exchanged) {
            *index = next;
            return A20_OK;
        }
        next = observed;
    }
}
#endif

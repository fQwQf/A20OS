#ifndef A20_NATIVE_BALANCE_CONTRACT_H
#define A20_NATIVE_BALANCE_CONTRACT_H
#include "liba20rt/a20_task.h"
#include "liba20rt/a20_execution.h"
#include "liba20rt/a20_work.h"
#ifdef A20_NATIVE_FILE_TEST
#include "native-file-contract.h"
#endif
/* Two real user threads compute disjoint parts of one shared input. Neither
 * work thread is assigned a CPU: both admit the complete online domain. */
static struct {
    volatile uint32_t input[128];
    uint32_t online, mask[2], jobs[2], sum[2], taken[2], ready[2], error[2];
    a20_work_range_t work;
} native_balance_shared;
static int native_balance_prepare(a20_handle_t worker) {
    a20_execution_domain_info_t domain;
    if (a20_execution_domain_info(&domain) ||
        !a20_execution_domain_valid(&domain))
        return 1;
    /* Empty, invalid and non-wrapping upper-bound ranges use the same SDK. */
    a20_work_range_t edge;
    uint32_t index = 0xa20;
    if (a20_work_range_init(&edge, 0xffffffffU - 1, 0xffffffffU) ||
        a20_work_range_take(&edge, &index) || index != 0xffffffffU - 1 ||
        a20_work_range_take(&edge, &index) != -A20_ERR_WOULD_BLOCK ||
        index != 0xffffffffU - 1 ||
        a20_work_range_init(&edge, 2, 1) != -A20_ERR_INVALID_ARGUMENT ||
        a20_work_range_init(&edge, 0, 0) ||
        a20_work_range_take(&edge, &index) != -A20_ERR_WOULD_BLOCK ||
        index != 0xffffffffU - 1 ||
        a20_work_range_init(&native_balance_shared.work, 0, 32))
        return 2;
    native_balance_shared.online = (uint32_t)domain.online_mask;
    for (unsigned i = 0; i < 128; i++)
        native_balance_shared.input[i] = i + 1;
    for (unsigned i = 0; i < 2; i++) {
        native_balance_shared.mask[i] = 0;
        native_balance_shared.jobs[i] = 0;
        native_balance_shared.sum[i] = 0;
        native_balance_shared.taken[i] = 0;
        native_balance_shared.ready[i] = 0;
        native_balance_shared.error[i] = 0;
    }
    a20_sched_args_t sched = {.flags = A20_SCHED_AFFINITY,
                              .affinity = domain.online_mask,
                              .affinity_size = sizeof(uint64_t)};
    return a20_sched_set(worker, &sched) != A20_OK;
}
/* Publish failures too: an exited peer cannot satisfy a success barrier. */
static int native_balance_wait(unsigned part, uint32_t threshold) {
    for (;;) {
        uint32_t ready, error;
        if (a20_atomic32_load(&native_balance_shared.error[1 - part], &error) ||
            error ||
            a20_atomic32_load(&native_balance_shared.ready[1 - part], &ready))
            return 1;
        if (ready >= threshold)
            return 0;
        if (a20_thread_yield())
            return 1;
    }
}
static int native_balance_work_inner(unsigned part) {
    if (a20_atomic32_store(&native_balance_shared.ready[part], 1))
        return 5;
    if (native_balance_wait(part, 1))
        return 6;
#ifdef A20_NATIVE_FILE_TEST
    /* Both workers retain ownership throughout every file operation. A closed
     * handle number can be reused by the other worker immediately. */
    int file_result = native_file_contract(0);
    if (file_result)
        return 100 + file_result;
    if (a20_atomic32_store(&native_balance_shared.ready[part], 2))
        return 8;
    if (native_balance_wait(part, 2))
        return 9;
#endif
    for (;;) {
        uint32_t job;
        a20_status_t status =
            a20_work_range_take(&native_balance_shared.work, &job);
        if (status == -A20_ERR_WOULD_BLOCK)
            break;
        if (status || job >= 32 ||
            (native_balance_shared.taken[part] & (1U << job)))
            return 7;
        uint32_t before, after;
        if (a20_thread_get_cpu(&before) || before >= 32 ||
            !(native_balance_shared.online & (1U << before)))
            return 2;
        uint32_t sum = 0;
        for (unsigned i = 0; i < 128; i++)
            sum += native_balance_shared.input[i] * (job + 1);
        if (sum != 8256 * (job + 1) || a20_thread_get_cpu(&after) ||
            after >= 32 || !(native_balance_shared.online & (1U << after)))
            return 3;
        native_balance_shared.mask[part] |= (1U << before) | (1U << after);
        native_balance_shared.sum[part] += sum;
        native_balance_shared.jobs[part]++;
        native_balance_shared.taken[part] |= 1U << job;
        if (a20_thread_yield())
            return 4;
    }
    return 0;
}
static int native_balance_work(unsigned part) {
    if (part >= 2)
        return 1;
    int result = native_balance_work_inner(part);
    if (result && a20_atomic32_store(&native_balance_shared.error[part],
                                     (uint32_t)result))
        return 10;
    return result;
}
static uint32_t native_balance_mask(void) {
    return native_balance_shared.mask[0] | native_balance_shared.mask[1];
}
static int native_balance_result(void) {
    uint32_t mask = native_balance_mask();
    return native_balance_shared.jobs[0] + native_balance_shared.jobs[1] ==
               32 &&
           native_balance_shared.sum[0] + native_balance_shared.sum[1] ==
               4359168 &&
           !(native_balance_shared.taken[0] & native_balance_shared.taken[1]) &&
           (native_balance_shared.taken[0] | native_balance_shared.taken[1]) ==
               0xffffffffU &&
           !(mask & ~native_balance_shared.online) && mask &&
           ((native_balance_shared.online & (native_balance_shared.online - 1))
                ? !!(mask & (mask - 1))
                : mask == native_balance_shared.online);
}
#endif

/* SPDX-License-Identifier: Apache-2.0 */
#ifndef CONFIG_MCU
#include "drivers/core/dma_mapping.h"
#include "core/errno.h"
#include "core/refcount.h"
#include "mm/slab.h"

struct driver_dma_policy {
    refcount_t references;
    size_t count;
    struct driver_dma_rule rules[];
};

int driver_dma_policy_create(const struct driver_dma_rule *rules, size_t count,
                              struct driver_dma_policy **out) {
    if (!out)
        return -EINVAL;
    *out = NULL;
    if (count && !rules)
        return -EINVAL;
    if (count > DRIVER_DMA_POLICY_MAX_RULES)
        return -E2BIG;
    for (size_t i = 0; i < count; i++)
        if (rules[i].start > rules[i].end ||
            (rules[i].access & ~(DRIVER_DMA_POLICY_READ | DRIVER_DMA_POLICY_WRITE)) ||
            (rules[i].flags & ~DRIVER_DMA_POLICY_IDENTITY))
            return -EINVAL;
    struct driver_dma_policy *policy =
        kmalloc(sizeof(*policy) + count * sizeof(*rules));
    if (!policy)
        return -ENOMEM;
    refcount_set(&policy->references, 1);
    policy->count = count;
    /* Bounded creation-time sort. Queries can stop before later intervals;
     * overlapping rules deliberately retain their independent restrictions. */
    for (size_t i = 0; i < count; i++) {
        size_t at = i;
        while (at && policy->rules[at - 1].start > rules[i].start) {
            policy->rules[at] = policy->rules[at - 1];
            at--;
        }
        policy->rules[at] = rules[i];
    }
    *out = policy;
    return 0;
}

void driver_dma_policy_get(struct driver_dma_policy *policy) {
    if (policy)
        refcount_inc(&policy->references);
}

void driver_dma_policy_put(struct driver_dma_policy *policy) {
    if (policy && refcount_dec_and_test(&policy->references))
        kfree(policy);
}

int driver_dma_policy_check(const struct driver_dma_policy *policy,
                             uint64_t physical, uint64_t length,
                             enum driver_dma_direction direction,
                             bool *identity) {
    if (!identity)
        return -EINVAL;
    *identity = false;
    if (!policy || !length || physical > ~(uint64_t)0 - (length - 1))
        return -EINVAL;
    unsigned access;
    if (direction == DRIVER_DMA_TO_DEVICE)
        access = DRIVER_DMA_POLICY_READ;
    else if (direction == DRIVER_DMA_FROM_DEVICE)
        access = DRIVER_DMA_POLICY_WRITE;
    else if (direction == DRIVER_DMA_BIDIRECTIONAL)
        access = DRIVER_DMA_POLICY_READ | DRIVER_DMA_POLICY_WRITE;
    else
        return -EINVAL;
    uint64_t end = physical + length - 1;
    bool required = false;
    for (size_t i = 0; i < policy->count; i++) {
        const struct driver_dma_rule *rule = &policy->rules[i];
        if (rule->start > end)
            break;
        if (rule->end < physical)
            continue;
        if ((rule->access & access) != access)
            return -EACCES;
        required |= (rule->flags & DRIVER_DMA_POLICY_IDENTITY) != 0;
    }
    *identity = required;
    return 0;
}
#endif

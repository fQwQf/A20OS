/* SPDX-License-Identifier: Apache-2.0 */
/* Reuse only allocation/frame/provider fixtures; all policy and mapping
 * decisions are the separately compiled production implementations. */
#define main previous_mapping_fixture_main
#include "driver_dma_mapping_host.c"
#undef main

static struct driver_dma_policy *policy_create(const struct driver_dma_rule *r,
                                                size_t count)
{
    struct driver_dma_policy *p = NULL;
    assert(!driver_dma_policy_create(r, count, &p) && p);
    return p;
}

static void policy_check(struct driver_dma_policy *p, uint64_t physical,
                           uint64_t length, enum driver_dma_direction direction,
                           int error, bool expected_identity)
{
    bool identity = !expected_identity;
    assert(driver_dma_policy_check(p, physical, length, direction, &identity) == error);
    assert(identity == expected_identity);
}

static void policy_arguments(void)
{
    setup();
    struct driver_dma_rule r = {2, 1, 3, 0};
    struct driver_dma_policy *p = (void *)1;
    assert(driver_dma_policy_create(&r, 1, &p) == -EINVAL && !p);
    r = (struct driver_dma_rule){0, 1, 4, 0};
    assert(driver_dma_policy_create(&r, 1, &p) == -EINVAL && !p);
    r = (struct driver_dma_rule){0, 1, 3, 2};
    assert(driver_dma_policy_create(&r, 1, &p) == -EINVAL && !p);
    assert(driver_dma_policy_create(NULL, 1, &p) == -EINVAL && !p);
    assert(driver_dma_policy_create(&r, DRIVER_DMA_POLICY_MAX_RULES + 1, &p) == -E2BIG && !p);
    assert(driver_dma_policy_create(NULL, 0, NULL) == -EINVAL);
    fail_alloc = 1;
    assert(driver_dma_policy_create(NULL, 0, &p) == -ENOMEM && !p);
    p = policy_create(NULL, 0);
    policy_check(p, ~(uint64_t)0, 1, DRIVER_DMA_BIDIRECTIONAL, 0, false);
    policy_check(p, ~(uint64_t)0, 2, DRIVER_DMA_TO_DEVICE, -EINVAL, false);
    policy_check(p, 0, 0, DRIVER_DMA_TO_DEVICE, -EINVAL, false);
    policy_check(p, 0, 1, 99, -EINVAL, false);
    policy_check(NULL, 0, 1, DRIVER_DMA_TO_DEVICE, -EINVAL, false);
    assert(driver_dma_policy_check(p, 0, 1, DRIVER_DMA_TO_DEVICE, NULL) == -EINVAL);
    driver_dma_policy_get(p); driver_dma_policy_put(p);
    policy_check(p, 0, 1, DRIVER_DMA_FROM_DEVICE, 0, false);
    driver_dma_policy_put(p); driver_dma_policy_get(NULL); driver_dma_policy_put(NULL);
    assert(!allocations);
}

static void copied_intersections(void)
{
    setup();
    struct driver_dma_rule rules[] = {
        {0x400800, 0x4017ff, DRIVER_DMA_POLICY_READ, 0},
        {0x400000, 0x400fff, 3, DRIVER_DMA_POLICY_IDENTITY},
        {0x401000, 0x401fff, DRIVER_DMA_POLICY_WRITE, 0},
        {~(uint64_t)0, ~(uint64_t)0, 0, 0},
    };
    struct driver_dma_policy *p = policy_create(rules, 4);
    memset(rules, 0, sizeof(rules)); /* The caller storage is no longer used. */
    policy_check(p, 0x400000, 1, DRIVER_DMA_BIDIRECTIONAL, 0, true);
    policy_check(p, 0x400800, 1, DRIVER_DMA_TO_DEVICE, 0, true);
    policy_check(p, 0x400800, 1, DRIVER_DMA_FROM_DEVICE, -EACCES, false);
    policy_check(p, 0x400fff, 2, DRIVER_DMA_TO_DEVICE, -EACCES, false);
    policy_check(p, 0x401000, 1, DRIVER_DMA_FROM_DEVICE, -EACCES, false);
    policy_check(p, 0x401800, 0x800, DRIVER_DMA_FROM_DEVICE, 0, false);
    policy_check(p, 0x401800, 0x800, DRIVER_DMA_TO_DEVICE, -EACCES, false);
    policy_check(p, 0x402000, 1, DRIVER_DMA_BIDIRECTIONAL, 0, false);
    policy_check(p, ~(uint64_t)0 - 1, 1, DRIVER_DMA_BIDIRECTIONAL, 0, false);
    policy_check(p, ~(uint64_t)0, 1, DRIVER_DMA_TO_DEVICE, -EACCES, false);
    driver_dma_policy_put(p); assert(!allocations);
}

static void maximum_policy(void)
{
    setup();
    struct driver_dma_rule rules[DRIVER_DMA_POLICY_MAX_RULES];
    for (unsigned i = 0; i < DRIVER_DMA_POLICY_MAX_RULES; i++) {
        uint64_t at = DRIVER_DMA_POLICY_MAX_RULES - 1 - i;
        rules[i] = (struct driver_dma_rule){at, at, i ? 3 : 0, 0};
    }
    struct driver_dma_policy *p = policy_create(rules, DRIVER_DMA_POLICY_MAX_RULES);
    policy_check(p, 0, 1, DRIVER_DMA_BIDIRECTIONAL, 0, false);
    policy_check(p, DRIVER_DMA_POLICY_MAX_RULES - 1, 1, DRIVER_DMA_TO_DEVICE, -EACCES, false);
    policy_check(p, DRIVER_DMA_POLICY_MAX_RULES, 1, DRIVER_DMA_TO_DEVICE, 0, false);
    driver_dma_policy_put(p); assert(!allocations);
}

static unsigned policy_random = 0x82d41;
static unsigned next_policy_random(void)
{
    policy_random = policy_random * 1664525U + 1013904223U;
    return policy_random;
}

static void byte_model(void)
{
    setup();
    unsigned seen_access = 0, seen_flags = 0, allowed_queries = 0, denied_queries = 0, identity_queries = 0;
    for (unsigned round = 0; round < 120; round++) {
        struct driver_dma_rule rules[12];
        unsigned count = round % 9;
        for (unsigned i = 0; i < count; i++) {
            unsigned a = (next_policy_random() >> 16) % 64;
            unsigned b = a + (next_policy_random() >> 16) % 12;
            rules[i] = (struct driver_dma_rule){a, b < 64 ? b : 63,
                (next_policy_random() >> 16) % 4, (next_policy_random() >> 16) % 2};
            seen_access |= 1U << rules[i].access;
            seen_flags |= 1U << rules[i].flags;
        }
        struct driver_dma_policy *p = policy_create(rules, count);
        for (unsigned start = 0; start < 64; start++) {
            for (unsigned length = 1; length <= 64 - start; length += 7) {
                for (unsigned direction = 0; direction < 3; direction++) {
                    unsigned needed = direction == DRIVER_DMA_TO_DEVICE ? 1 :
                                      direction == DRIVER_DMA_FROM_DEVICE ? 2 : 3;
                    bool denied = false, identity = false;
                    /* Independent oracle: inspect permissions at EACH BYTE,
                     * rather than production's interval-intersection query. */
                    for (unsigned byte = start; byte < start + length; byte++) {
                        unsigned allowed = 3;
                        for (unsigned i = 0; i < count; i++) {
                            if (byte < rules[i].start || byte > rules[i].end) continue;
                            allowed &= rules[i].access;
                            identity |= rules[i].flags != 0;
                        }
                        denied |= (allowed & needed) != needed;
                    }
                    policy_check(p, start, length, direction,
                                   denied ? -EACCES : 0, !denied && identity);
                    if (denied) denied_queries++;
                    else { allowed_queries++; identity_queries += identity; }
                }
            }
        }
        driver_dma_policy_put(p);
    }
    assert(seen_access == 15 && seen_flags == 3);
    assert(allowed_queries > 100 && denied_queries > 100 && identity_queries > 100);
    assert(!allocations);
}

struct policy_provider {
    struct provider base;
    struct driver_dma_policy *borrowed;
};
static void policy_release(void *opaque)
{
    struct policy_provider *p = opaque;
    /* Domain policy must remain alive through provider release(), even when
     * the constructor's caller dropped its own reference long ago. */
    policy_check(p->borrowed, 0, 1, DRIVER_DMA_TO_DEVICE, 0, false);
    release(&p->base);
}
static const struct driver_dma_managed_ops policy_ops = {
    managed_map, checked_unmap, deactivate, policy_release,
};

static void domain_failure_and_preflight(void)
{
    setup();
    struct driver_dma_rule rule = {0x403008, 0x403008, 0, 0};
    struct driver_dma_policy *policy = policy_create(&rule, 1);
    device_t d = {.references = 1};
    struct policy_provider p = {.base.offset = 0x200000, .borrowed = policy};
    struct driver_dma_domain *domain = (void *)1;
    assert(driver_dma_domain_create_managed_policy(&d, &policy_ops, &p, NULL, &domain) == -EINVAL && !domain);
    assert(driver_dma_domain_create_managed_policy(&d, NULL, &p, policy, &domain) == -EINVAL && !domain);
    fail_alloc = 1;
    assert(driver_dma_domain_create_managed_policy(&d, &policy_ops, &p, policy, &domain) == -ENOMEM && !domain);
    assert(d.references == 1 && !p.base.releases && allocations == 1);
    assert(!driver_dma_domain_create_managed_policy(&d, &policy_ops, &p, policy, &domain));
    driver_dma_policy_put(policy); /* Only the domain now retains the policy. */
    pfn_t pages[] = {100, 103, 112};
    struct driver_dma_mapping *mapping = (void *)1;
    assert(driver_dma_map_pages(domain, pages, 3, DRIVER_DMA_TO_DEVICE, &mapping) == -EACCES && !mapping);
    assert(!p.base.attempts && !device_syncs && refs[100] == 1 && refs[103] == 1 && refs[112] == 1);
    assert(!driver_dma_map_pages(domain, pages, 1, DRIVER_DMA_TO_DEVICE, &mapping));
    assert(driver_dma_domain_shutdown(domain) == -EBUSY);
    assert(!driver_dma_try_unmap_pages(&mapping));
    assert(!driver_dma_domain_shutdown(domain)); driver_dma_domain_put(domain);
    assert(!allocations && d.references == 1 && p.base.releases == 1);
}

static void directional_access(void)
{
    setup();
    struct driver_dma_rule rule = {0x400fff, 0x401000, DRIVER_DMA_POLICY_READ, 0};
    struct driver_dma_policy *policy = policy_create(&rule, 1);
    device_t d = {.references = 1};
    struct policy_provider p = {.borrowed = policy};
    struct driver_dma_domain *domain;
    assert(!driver_dma_domain_create_managed_policy(&d, &policy_ops, &p, policy, &domain));
    driver_dma_policy_put(policy);
    pfn_t pages[] = {100, 101}; struct driver_dma_mapping *mapping;
    assert(driver_dma_map_pages(domain, pages, 2, DRIVER_DMA_FROM_DEVICE, &mapping) == -EACCES && !mapping);
    assert(driver_dma_map_pages(domain, pages, 2, DRIVER_DMA_BIDIRECTIONAL, &mapping) == -EACCES && !mapping);
    assert(!p.base.attempts);
    assert(!driver_dma_map_pages(domain, pages, 2, DRIVER_DMA_TO_DEVICE, &mapping));
    assert(!driver_dma_try_unmap_pages(&mapping));
    assert(!driver_dma_domain_shutdown(domain)); driver_dma_domain_put(domain);
    assert(!allocations && refs[100] == 1 && refs[101] == 1);
}

static void identity_and_uncertain_cleanup(bool fail_cleanup)
{
    setup();
    struct driver_dma_rule rule = {0x400001, 0x400001, 3, DRIVER_DMA_POLICY_IDENTITY};
    struct driver_dma_policy *policy = policy_create(&rule, 1);
    device_t d = {.references = 1};
    struct policy_provider p = {.base.offset = 0x200000, .borrowed = policy};
    if (fail_cleanup) p.base.unmap_fail_at = 1;
    struct driver_dma_domain *domain;
    assert(!driver_dma_domain_create_managed_policy(&d, &policy_ops, &p, policy, &domain));
    driver_dma_policy_put(policy);
    pfn_t page = 100; struct driver_dma_mapping *mapping;
    assert(driver_dma_map_pages(domain, &page, 1, DRIVER_DMA_TO_DEVICE, &mapping) == -ERANGE);
    assert(p.base.attempts == 1 && !device_syncs && !cpu_syncs);
    if (fail_cleanup) {
        assert(mapping && refs[page] == 2 && driver_dma_mapping_count(mapping) == 1);
        uint64_t address;
        assert(driver_dma_page_address(mapping, 0, &address) == -ESHUTDOWN);
        assert(driver_dma_mapping_sync_for_device(mapping) == -ESHUTDOWN);
        assert(driver_dma_domain_shutdown(domain) == -EBUSY);
        assert(!driver_dma_try_unmap_pages(&mapping));
        p.base.detach_error = -ETIMEDOUT;
        assert(driver_dma_domain_shutdown(domain) == -ETIMEDOUT && allocations == 2);
        p.base.detach_error = 0;
    } else assert(!mapping);
    assert(refs[page] == 1);
    assert(!driver_dma_domain_shutdown(domain)); driver_dma_domain_put(domain);
    assert(!allocations && p.base.releases == 1 && d.references == 1);
}

static void identity_success(void)
{
    setup();
    struct driver_dma_rule rule = {0x400000, 0x400fff, 3, DRIVER_DMA_POLICY_IDENTITY};
    struct driver_dma_policy *policy = policy_create(&rule, 1);
    device_t d = {.references = 1}; struct policy_provider p = {.borrowed = policy};
    struct driver_dma_domain *domain;
    assert(!driver_dma_domain_create_managed_policy(&d, &policy_ops, &p, policy, &domain));
    driver_dma_policy_put(policy);
    pfn_t page = 100; struct driver_dma_mapping *mapping; uint64_t address;
    assert(!driver_dma_map_pages(domain, &page, 1, DRIVER_DMA_BIDIRECTIONAL, &mapping));
    assert(!driver_dma_page_address(mapping, 0, &address) && address == 0x400000);
    assert(!driver_dma_try_unmap_pages(&mapping));
    assert(!driver_dma_domain_shutdown(domain)); driver_dma_domain_put(domain);
    assert(!allocations && refs[page] == 1);
}

int main(void)
{
    policy_arguments(); copied_intersections(); maximum_policy(); byte_model();
    domain_failure_and_preflight(); directional_access();
    identity_and_uncertain_cleanup(false); identity_and_uncertain_cleanup(true);
    identity_success();
    puts("native DMA policy: byte model, admission, identity and retained cleanup PASS");
    return 0;
}

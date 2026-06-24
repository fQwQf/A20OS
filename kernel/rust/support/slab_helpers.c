#include "mm/frame.h"
#include "mm/oom.h"

pfn_t a20_pfa_alloc(int order)
{
    return pfa_alloc(order);
}

void a20_pfa_free(pfn_t pfn, int order)
{
    pfa_free(pfn, order);
}

void a20_pfa_free_page(pfn_t pfn)
{
    pfa_free_page(pfn);
}

pfn_t a20_virt_to_pfn(const void *va)
{
    return virt_to_pfn(va);
}

uint8_t a20_pfa_meta_flags(pfn_t pfn)
{
    if (!pfn_valid(pfn))
        return 0;
    return pfa.meta[pfn].flags;
}

uint16_t a20_pfa_meta_refcount(pfn_t pfn)
{
    if (!pfn_valid(pfn))
        return 0;
    return pfa.meta[pfn].refcount;
}

void a20_oom_try_reclaim(void)
{
    oom_try_reclaim();
}

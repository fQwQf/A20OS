/*
 * Adversarial host fixture for the production mm_tlb_release_holds helper.
 * The Python runner inserts the production enum, hold type, and function.
 */
#include <assert.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

typedef uint64_t pfn_t;
typedef struct page_cache_page { int marker; } page_cache_page_t;
#define PFN_NONE UINT64_MAX
#define ARCH_PT_ROOT_LEVEL 4
#define ARCH_PT_ROOT_ORDER 2

/* INSERT_PRODUCTION_MM_TLB_HOLD_DEFINITIONS */

static int ordinary_refs[4096];
static int reused_owner_refs[4096];
static unsigned pfa_free_calls;
static int pfa_free_orders[4];
static unsigned frame_put_many_calls;
static size_t frame_put_many_sizes[4];
static unsigned page_cache_put_calls;
static unsigned kfree_calls;

static void pfa_free(pfn_t pfn, int order)
{
    assert(pfn == 10 || pfn == 11);
    assert(pfa_free_calls < 4);
    pfa_free_orders[pfa_free_calls++] = order;

    /* Model immediate allocator reuse: a new owner takes the same PFN with
     * one reference before the deferred-hold drain proceeds to its next item. */
    assert(reused_owner_refs[pfn] == 0);
    reused_owner_refs[pfn] = 1;
}

static void frame_put_many(const pfn_t *pfns, size_t count)
{
    assert(frame_put_many_calls < 4);
    frame_put_many_sizes[frame_put_many_calls++] = count;
    for (size_t i = 0; i < count; ++i) {
        pfn_t pfn = pfns[i];
        if (pfn == 10 || pfn == 11) {
            /* The old bug put a just-reused page-table PFN into this path. */
            assert(reused_owner_refs[pfn] > 0);
            reused_owner_refs[pfn]--;
        } else {
            assert(pfn < 4096 && ordinary_refs[pfn] == 1);
            ordinary_refs[pfn]--;
        }
    }
}

static void page_cache_put(page_cache_page_t *page)
{
    assert(page != NULL);
    page_cache_put_calls++;
}

static void kfree(void *ptr)
{
    assert(ptr != NULL);
    kfree_calls++;
}

/* INSERT_PRODUCTION_MM_TLB_RELEASE_HELPER */

int main(void)
{
    enum { ORDINARY_COUNT = 65, HOLD_COUNT = ORDINARY_COUNT + 3 };
    mm_tlb_hold_t holds[HOLD_COUNT];
    page_cache_page_t page = {0};
    memset(holds, 0, sizeof(holds));

    /* List order deliberately interleaves both PT ownership classes with a
     * 65-frame run and a page-cache pin. */
    holds[0] = (mm_tlb_hold_t){ .frame = 10, .kind = MM_TLB_HOLD_PT,
                                .pt_level = ARCH_PT_ROOT_LEVEL };
    for (unsigned i = 0; i < ORDINARY_COUNT; ++i) {
        pfn_t pfn = 100 + i;
        ordinary_refs[pfn] = 1;
        holds[i + 1] = (mm_tlb_hold_t){ .frame = pfn,
                                        .kind = MM_TLB_HOLD_FRAME };
    }
    holds[ORDINARY_COUNT + 1] = (mm_tlb_hold_t){
        .frame = 11, .kind = MM_TLB_HOLD_PT, .pt_level = 0
    };
    holds[ORDINARY_COUNT + 2] = (mm_tlb_hold_t){
        .page = &page, .kind = MM_TLB_HOLD_PAGE
    };
    for (unsigned i = 0; i + 1 < HOLD_COUNT; ++i)
        holds[i].next = &holds[i + 1];

    mm_tlb_release_holds(&holds[0]);

    assert(pfa_free_calls == 2);
    assert(pfa_free_orders[0] == ARCH_PT_ROOT_ORDER);
    assert(pfa_free_orders[1] == 0);
    assert(reused_owner_refs[10] == 1 && reused_owner_refs[11] == 1);
    assert(frame_put_many_calls == 2);
    assert(frame_put_many_sizes[0] == 64 && frame_put_many_sizes[1] == 1);
    for (unsigned i = 0; i < ORDINARY_COUNT; ++i)
        assert(ordinary_refs[100 + i] == 0);
    assert(page_cache_put_calls == 1);
    assert(kfree_calls == HOLD_COUNT);

    puts("mm-tlb-hold-release: production helper ownership and batching PASS");
    return 0;
}

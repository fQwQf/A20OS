#ifndef _FRAME_H
#define _FRAME_H

#include "core/types.h"
#include "core/consts.h"
#include "core/arch.h"

/*
 * A20OS Buddy Physical Frame Allocator
 *
 * Manages physical memory frames using a buddy system.
 * O(1) allocation and free with automatic coalescing.
 * Supports contiguous allocation of 2^order frames (up to 8MB).
 * Per-frame reference counting for COW support.
 */

#define MAX_ORDER    11   /* orders 0..10 → 4KB to 8MB */
#define PFN_NONE     ((uint32_t)-1)
#define PFA_MAX_RANGES 4

typedef uint32_t pfn_t;

typedef struct {
    paddr_t base;
    paddr_t end;
    pfn_t   start_pfn;
    pfn_t   end_pfn;
} pfa_range_t;

/* Per-frame metadata.
 * prev/next live here instead of inside the free page itself, so a stray
 * write into a freed frame cannot corrupt the buddy free lists. */
typedef struct {
    uint16_t refcount;   /* 0 = free, >0 = in-use */
    uint8_t  order;      /* buddy order */
    uint8_t  flags;      /* FRAME_F_* */
    pfn_t    prev;       /* free-list prev (valid when FRAME_F_FREE) */
    pfn_t    next;       /* free-list next (valid when FRAME_F_FREE) */
} frame_meta_t;

#define FRAME_F_FREE    0x00
#define FRAME_F_ALLOC   0x01
#define FRAME_F_KDATA   0x02
#define FRAME_F_PT      0x04

/* Free-list head per order */
typedef struct {
    pfn_t  head;
    size_t count;
} free_list_t;

/* Global allocator state */
typedef struct {
    frame_meta_t *meta;
    pfn_t   total_frames;
    pfn_t   free_frames;
    size_t  nr_ranges;
    pfa_range_t ranges[PFA_MAX_RANGES];
    free_list_t free_lists[MAX_ORDER + 1];
} pfa_t;

extern pfa_t pfa;

/* Core API */
void   pfa_init(paddr_t kernel_end);
pfn_t  pfa_alloc(int order);
void   pfa_free(pfn_t pfn, int order);
pfn_t  pfa_alloc_page(void);
void   pfa_free_page(pfn_t pfn);
void   frame_get(pfn_t pfn);
void   frame_put(pfn_t pfn);
size_t pfa_free_count(void);

// 内核使用恒等映射
// 虚拟地址 (VA)，物理地址 (PA)和页帧号 (PFN) 的辅助函数

static inline const pfa_range_t *pfa_range_for_pfn(pfn_t pfn) {
    for (size_t i = 0; i < pfa.nr_ranges; i++) {
        const pfa_range_t *range = &pfa.ranges[i];
        if (pfn >= range->start_pfn && pfn < range->end_pfn)
            return range;
    }
    return NULL;
}

static inline const pfa_range_t *pfa_range_for_pa(paddr_t pa) {
    for (size_t i = 0; i < pfa.nr_ranges; i++) {
        const pfa_range_t *range = &pfa.ranges[i];
        if (pa >= range->base && pa < range->end)
            return range;
    }
    return NULL;
}

static inline void *pfn_to_virt(pfn_t pfn) {
    const pfa_range_t *range = pfa_range_for_pfn(pfn);
    if (!range) return NULL;
    return (void *)(PAGE_OFFSET + range->base +
                    (((paddr_t)(pfn - range->start_pfn)) << PAGE_SIZE_BITS));
}

static inline paddr_t pfn_to_phys(pfn_t pfn) {
    const pfa_range_t *range = pfa_range_for_pfn(pfn);
    if (!range) return (paddr_t)-1;
    return range->base + (((paddr_t)(pfn - range->start_pfn)) << PAGE_SIZE_BITS);
}

static inline pfn_t phys_to_pfn(paddr_t pa) {
    const pfa_range_t *range = pfa_range_for_pa(pa);
    if (!range) return PFN_NONE;
    return range->start_pfn + (pfn_t)((pa - range->base) >> PAGE_SIZE_BITS);
}

static inline int pfn_valid(pfn_t pfn) {
    return pfa_range_for_pfn(pfn) != NULL;
}

static inline pfn_t virt_to_pfn(const void *va) {
    paddr_t pa = (paddr_t)(uintptr_t)va - PAGE_OFFSET;
    return phys_to_pfn(pa);
}

#endif /* _FRAME_H */

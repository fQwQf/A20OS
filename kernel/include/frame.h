#ifndef _FRAME_H
#define _FRAME_H

#include "types.h"
#include "consts.h"
#include "arch.h"

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

typedef uint32_t pfn_t;

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
    free_list_t free_lists[MAX_ORDER + 1];
} pfa_t;

extern pfa_t pfa;

/* Core API */
void   pfa_init(paddr_t ram_base, paddr_t ram_end, paddr_t kernel_end);
pfn_t  pfa_alloc(int order);
void   pfa_free(pfn_t pfn, int order);
pfn_t  pfa_alloc_page(void);
void   pfa_free_page(pfn_t pfn);
void   frame_get(pfn_t pfn);
void   frame_put(pfn_t pfn);
size_t pfa_free_count(void);

// 内核使用恒等映射
// 虚拟地址 (VA)，物理地址 (PA)和页帧号 (PFN) 的辅助函数

static inline void *pfn_to_virt(pfn_t pfn) {
    return (void *)(PAGE_OFFSET + PHYS_MEMORY_BASE + ((paddr_t)pfn << PAGE_SIZE_BITS));
}

static inline pfn_t virt_to_pfn(const void *va) {
    paddr_t pa = (paddr_t)(uintptr_t)va - PAGE_OFFSET;
    return (pfn_t)((pa - PHYS_MEMORY_BASE) >> PAGE_SIZE_BITS);
}

static inline paddr_t pfn_to_phys(pfn_t pfn) {
    return PHYS_MEMORY_BASE + ((paddr_t)pfn << PAGE_SIZE_BITS);
}

static inline pfn_t phys_to_pfn(paddr_t pa) {
    return (pfn_t)((pa - PHYS_MEMORY_BASE) >> PAGE_SIZE_BITS);
}

static inline int pfn_valid(pfn_t pfn) {
    return pfn < pfa.total_frames;
}

#endif /* _FRAME_H */

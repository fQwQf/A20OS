#include "frame.h"
#include "panic.h"
#include "string.h"
#include "stdio.h"

pfa_t pfa;

/* Free-block link stored inside the frame's own memory (zero overhead) */
typedef struct free_node {
    pfn_t prev;
    pfn_t next;
} free_node_t;

static inline free_node_t *node_of(pfn_t pfn) {
    return (free_node_t *)pfn_to_virt(pfn);
}

static void fl_push(pfn_t pfn, int order) {
    free_node_t *n = node_of(pfn);
    n->prev = PFN_NONE;
    n->next = pfa.free_lists[order].head;
    if (n->next != PFN_NONE)
        node_of(n->next)->prev = pfn;
    pfa.free_lists[order].head = pfn;
    pfa.free_lists[order].count++;
}

static void fl_remove(pfn_t pfn, int order) {
    free_node_t *n = node_of(pfn);
    if (n->prev != PFN_NONE)
        node_of(n->prev)->next = n->next;
    else
        pfa.free_lists[order].head = n->next;
    if (n->next != PFN_NONE)
        node_of(n->next)->prev = n->prev;
    pfa.free_lists[order].count--;
}

void pfa_init(paddr_t ram_base, paddr_t ram_end, paddr_t kernel_end) {
    pfa.total_frames = (pfn_t)((ram_end - ram_base) >> PAGE_SIZE_BITS);

    size_t meta_sz = (size_t)pfa.total_frames * sizeof(frame_meta_t);
    paddr_t meta_pa = ROUND_UP(kernel_end, 64);
    frame_meta_t *meta = (frame_meta_t *)((uint64_t)meta_pa + PAGE_OFFSET);
    memset(meta, 0, meta_sz);
    pfa.meta = meta;
    pfa.free_frames = 0;

    for (int i = 0; i <= MAX_ORDER; i++) {
        pfa.free_lists[i].head = PFN_NONE;
        pfa.free_lists[i].count = 0;
    }

    paddr_t used_end_pa = meta_pa + meta_sz;
    pfn_t used_pfn = (pfn_t)((ROUND_UP(used_end_pa, PAGE_SIZE) - ram_base) >> PAGE_SIZE_BITS);
    if (used_pfn > pfa.total_frames) used_pfn = pfa.total_frames;
    for (pfn_t i = 0; i < used_pfn; i++) {
        meta[i].flags    = FRAME_F_KDATA;
        meta[i].refcount = 1;
        meta[i].order    = 0;
    }

    /* Add remaining frames to buddy free lists.
     * Align start to MAX_ORDER boundary for clean splitting. */
    pfn_t start = (pfn_t)ROUND_UP((uint64_t)used_pfn, 1u << MAX_ORDER);

    /* Frames between used_pfn and aligned start → order 0 */
    for (pfn_t i = used_pfn; i < start && i < pfa.total_frames; i++) {
        meta[i].flags = FRAME_F_FREE;
        meta[i].order = 0;
        fl_push(i, 0);
        pfa.free_frames++;
    }

    /* Remaining frames as power-of-2 blocks from high order down */
    pfn_t remain = (pfa.total_frames > start) ? pfa.total_frames - start : 0;
    for (int o = MAX_ORDER; o >= 0 && remain > 0; o--) {
        pfn_t sz = 1u << o;
        while (remain >= sz) {
            meta[start].flags = FRAME_F_FREE;
            fl_push(start, o);
            start  += sz;
            remain -= sz;
            pfa.free_frames += sz;
        }
    }
}

pfn_t pfa_alloc(int order) {
    if (order < 0 || order > MAX_ORDER) return PFN_NONE;

    int o;
    for (o = order; o <= MAX_ORDER; o++)
        if (pfa.free_lists[o].head != PFN_NONE)
            break;
    if (o > MAX_ORDER) {
        printf("[PFA] alloc failed order=%d free_frames=%zu\n", order, pfa.free_frames);
        return PFN_NONE;
    }

    pfn_t blk = pfa.free_lists[o].head;
    fl_remove(blk, o);

    while (o > order) {
        o--;
        pfn_t buddy = blk ^ (1u << o);
        pfa.meta[buddy].flags = FRAME_F_FREE;
        pfa.meta[buddy].order = (uint8_t)o;
        fl_push(buddy, o);
    }

    pfa.meta[blk].flags    = FRAME_F_ALLOC;
    pfa.meta[blk].refcount = 1;
    pfa.meta[blk].order    = (uint8_t)order;
    pfa.free_frames -= (1u << order);
    return blk;
}

void pfa_free(pfn_t pfn, int order) {
    if (pfn >= pfa.total_frames) return;
    if (pfa.meta[pfn].flags == FRAME_F_FREE) return;
    int actual_order = (int)pfa.meta[pfn].order;
    if (actual_order < 0 || actual_order > MAX_ORDER) actual_order = order;
    pfa.free_frames += (1u << actual_order);

    while (actual_order < MAX_ORDER) {
        pfn_t buddy = pfn ^ (1u << actual_order);
        if (buddy >= pfa.total_frames) break;
        if (pfa.meta[buddy].flags != FRAME_F_FREE) break;
        if (pfa.meta[buddy].order != (uint8_t)actual_order) break;

        fl_remove(buddy, actual_order);
        pfn = (pfn < buddy) ? pfn : buddy;
        actual_order++;
    }

    pfa.meta[pfn].flags = FRAME_F_FREE;
    pfa.meta[pfn].order = (uint8_t)actual_order;
    fl_push(pfn, actual_order);
}

pfn_t pfa_alloc_page(void) { return pfa_alloc(0); }
void  pfa_free_page(pfn_t pfn) { pfa_free(pfn, 0); }

void frame_get(pfn_t pfn) {
    if (pfn_valid(pfn)) pfa.meta[pfn].refcount++;
}

void frame_put(pfn_t pfn) {
    if (!pfn_valid(pfn)) return;
    if (pfa.meta[pfn].refcount > 0 && --pfa.meta[pfn].refcount == 0)
        pfa_free(pfn, pfa.meta[pfn].order);
}

size_t pfa_free_count(void) { return pfa.free_frames; }

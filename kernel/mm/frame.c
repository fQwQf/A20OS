#include "mm/frame.h"
#include "core/lock.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/stdio.h"
#include "mm/oom.h"


pfa_t pfa;

static inline frame_meta_t *meta_of(pfn_t pfn) {
    return &pfa.meta[pfn];
}

static const pfa_range_t *find_range_by_pa(paddr_t pa) {
    for (size_t i = 0; i < pfa.nr_ranges; i++) {
        const pfa_range_t *range = &pfa.ranges[i];
        if (pa >= range->base && pa < range->end)
            return range;
    }
    return NULL;
}

// Buddy System 各阶的空闲块的入队和出队
static void fl_push(pfn_t pfn, int order) {
    frame_meta_t *m = meta_of(pfn);
    m->prev = PFN_NONE;
    m->next = pfa.free_lists[order].head;
    if (m->next != PFN_NONE)
        meta_of(m->next)->prev = pfn;
    pfa.free_lists[order].head = pfn;
    pfa.free_lists[order].count++;
}

static void fl_remove(pfn_t pfn, int order) {
    frame_meta_t *m = meta_of(pfn);
    if (m->prev != PFN_NONE) {
        if (!pfn_valid(m->prev) || pfa.meta[m->prev].order != (uint8_t)order ||
            pfa.meta[m->prev].flags != FRAME_F_FREE) {
            printf("[PFA BUG] fl_remove(%u,o=%d): bad prev=%u flags=%u order=%u ref=%u\n",
                   (unsigned)pfn, order, (unsigned)m->prev,
                   pfn_valid(m->prev) ? pfa.meta[m->prev].flags : 0U,
                   pfn_valid(m->prev) ? pfa.meta[m->prev].order : 0U,
                   pfn_valid(m->prev) ? pfa.meta[m->prev].refcount : 0U);
            panic("pfa: corrupted prev link");
        }
        meta_of(m->prev)->next = m->next;
    }
    else
        pfa.free_lists[order].head = m->next;
    if (m->next != PFN_NONE) {
        if (!pfn_valid(m->next) || pfa.meta[m->next].order != (uint8_t)order ||
            pfa.meta[m->next].flags != FRAME_F_FREE) {
            printf("[PFA BUG] fl_remove(%u,o=%d): bad next=%u flags=%u order=%u ref=%u\n",
                   (unsigned)pfn, order, (unsigned)m->next,
                   pfn_valid(m->next) ? pfa.meta[m->next].flags : 0U,
                   pfn_valid(m->next) ? pfa.meta[m->next].order : 0U,
                   pfn_valid(m->next) ? pfa.meta[m->next].refcount : 0U);
            panic("pfa: corrupted next link");
        }
        meta_of(m->next)->prev = m->prev;
    }
    m->prev = PFN_NONE;
    m->next = PFN_NONE;
    pfa.free_lists[order].count--;
}

// Buddy 分配器初始化函数，将物理内存划分为可用页框并构建空闲链表
void pfa_init(paddr_t kernel_end) {
    pfa.nr_ranges = arch_ram_range_count();
    if (pfa.nr_ranges == 0 || pfa.nr_ranges > PFA_MAX_RANGES)
        panic("pfa_init: invalid ram range count");

    pfa.total_frames = 0;
    for (size_t i = 0; i < pfa.nr_ranges; i++) {
        paddr_t base = 0, end = 0;
        if (arch_ram_range(i, &base, &end) < 0 || end <= base || (base & (PAGE_SIZE - 1)) ||
            (end & (PAGE_SIZE - 1)))
            panic("pfa_init: invalid ram range");

        pfa.ranges[i].base = base;
        pfa.ranges[i].end = end;
        pfa.ranges[i].start_pfn = pfa.total_frames;
        pfa.total_frames += (pfn_t)((end - base) >> PAGE_SIZE_BITS);
        pfa.ranges[i].end_pfn = pfa.total_frames;
    }

    // 分配元数据区
    size_t meta_sz = (size_t)pfa.total_frames * sizeof(frame_meta_t);
    paddr_t meta_pa = ROUND_UP(kernel_end, 64);
    const pfa_range_t *kernel_range = find_range_by_pa(meta_pa);
    if (!kernel_range || meta_pa + meta_sz > kernel_range->end)
        panic("pfa_init: frame metadata does not fit in kernel ram range");

    frame_meta_t *meta = (frame_meta_t *)(meta_pa + PAGE_OFFSET);
    memset(meta, 0, meta_sz);
    pfa.meta = meta;
    pfa.free_frames = 0;
    spin_init(&pfa.lock);

    for (int i = 0; i <= MAX_ORDER; i++) {
        pfa.free_lists[i].head = PFN_NONE;
        pfa.free_lists[i].count = 0;
    }

    // 标记内核已占用页并构建各 RAM 段自己的空闲链表
    paddr_t used_end_pa = ROUND_UP(meta_pa + meta_sz, PAGE_SIZE);
    for (size_t r = 0; r < pfa.nr_ranges; r++) {
        const pfa_range_t *range = &pfa.ranges[r];
        pfn_t range_frames = range->end_pfn - range->start_pfn;
        pfn_t used_frames = 0;

        if (meta_pa >= range->base && meta_pa < range->end) {
            used_frames = (pfn_t)((used_end_pa - range->base) >> PAGE_SIZE_BITS);
            if (used_frames > range_frames)
                used_frames = range_frames;
        }

        for (pfn_t i = range->start_pfn; i < range->start_pfn + used_frames; i++) {
            meta[i].flags    = FRAME_F_KDATA;
            meta[i].refcount = 1;
            meta[i].order    = 0;
            meta[i].prev     = PFN_NONE;
            meta[i].next     = PFN_NONE;
        }

        pfn_t start = (pfn_t)ROUND_UP((uint64_t)(range->start_pfn + used_frames), 1u << MAX_ORDER);

        // 处理对齐产生的碎片
        for (pfn_t i = range->start_pfn + used_frames; i < start && i < range->end_pfn; i++) {
            meta[i].flags = FRAME_F_FREE;
            meta[i].refcount = 0;
            meta[i].order = 0;
            meta[i].prev = PFN_NONE;
            meta[i].next = PFN_NONE;
            fl_push(i, 0);
            pfa.free_frames++;
        }

        // 剩余内存以大块形式放入链表
        pfn_t remain = (range->end_pfn > start) ? range->end_pfn - start : 0;
        for (int o = MAX_ORDER; o >= 0 && remain > 0; o--) {
            pfn_t sz = 1u << o;
            while (remain >= sz) {
                meta[start].flags = FRAME_F_FREE;
                meta[start].refcount = 0;
                meta[start].order = (uint8_t)o;
                meta[start].prev = PFN_NONE;
                meta[start].next = PFN_NONE;
                fl_push(start, o);
                start  += sz;
                remain -= sz;
                pfa.free_frames += sz;
            }
        }
    }
}

// Try to satisfy a high-order allocation by splitting lower-order blocks.
// Called with pfa.lock held. Returns PFN_NONE or a valid pfn.
static pfn_t pfa_alloc_from_buddy(int order)
{
    int o;
    for (o = order; o <= MAX_ORDER; o++)
        if (pfa.free_lists[o].head != PFN_NONE)
            break;
    if (o > MAX_ORDER)
        return PFN_NONE;

    pfn_t blk = pfa.free_lists[o].head;
    fl_remove(blk, o);

    while (o > order) {
        o--;
        pfn_t buddy = blk ^ (1u << o);
        pfa.meta[buddy].flags = FRAME_F_FREE;
        pfa.meta[buddy].refcount = 0;
        pfa.meta[buddy].order = (uint8_t)o;
        pfa.meta[buddy].prev = PFN_NONE;
        pfa.meta[buddy].next = PFN_NONE;
        fl_push(buddy, o);
    }

    pfa.meta[blk].flags    = FRAME_F_ALLOC;
    pfa.meta[blk].refcount = 1;
    pfa.meta[blk].order    = (uint8_t)order;
    pfa.meta[blk].prev     = PFN_NONE;
    pfa.meta[blk].next     = PFN_NONE;
    pfa.free_frames -= (1u << order);
    return blk;
}

pfn_t pfa_alloc(int order) {
    if (order < 0 || order > MAX_ORDER) return PFN_NONE;

    int retries = 0;
    while (retries < 2) {
        uint64_t flags = spin_lock_irqsave(&pfa.lock);
        pfn_t result = pfa_alloc_from_buddy(order);
        if (result != PFN_NONE) {
            spin_unlock_irqrestore(&pfa.lock, flags);
            return result;
        }
        spin_unlock_irqrestore(&pfa.lock, flags);

        if (retries == 0) {
            if (oom_try_reclaim()) {
                retries++;
                continue;
            }
        }
        break;
    }

    static size_t last_reported_free = (size_t)-1;
    size_t cur_free = pfa.free_frames;
    if (last_reported_free == (size_t)-1 ||
        (last_reported_free > cur_free && last_reported_free - cur_free >= 32)) {
        printf("[PFA] alloc failed order=%d free_frames=%lu\n",
               order, (unsigned long)cur_free);
        last_reported_free = cur_free;
    }

    return PFN_NONE;
}

void pfa_free(pfn_t pfn, int order) {
    if (pfn >= pfa.total_frames) return;

    uint64_t flags = spin_lock_irqsave(&pfa.lock);
    if (pfa.meta[pfn].flags == FRAME_F_FREE) {
        spin_unlock_irqrestore(&pfa.lock, flags);
        return;
    }

    int actual_order = (int)pfa.meta[pfn].order;
    if (actual_order < 0 || actual_order > MAX_ORDER) actual_order = order;
    pfa.free_frames += (1u << actual_order);
    const pfa_range_t *range = pfa_range_for_pfn(pfn);
    if (!range) {
        spin_unlock_irqrestore(&pfa.lock, flags);
        return;
    }

    // 试图向上合并
    while (actual_order < MAX_ORDER) {
        pfn_t buddy = pfn ^ (1u << actual_order);
        if (buddy >= pfa.total_frames) break;
        if (buddy < range->start_pfn || buddy >= range->end_pfn) break;
        if (pfa.meta[buddy].flags != FRAME_F_FREE) break;
        if (pfa.meta[buddy].order != (uint8_t)actual_order) break;

        fl_remove(buddy, actual_order);
        pfn = (pfn < buddy) ? pfn : buddy;
        actual_order++;
    }

    pfa.meta[pfn].flags = FRAME_F_FREE;
    pfa.meta[pfn].refcount = 0;
    pfa.meta[pfn].order = (uint8_t)actual_order;
    pfa.meta[pfn].prev = PFN_NONE;
    pfa.meta[pfn].next = PFN_NONE;
    fl_push(pfn, actual_order);
    spin_unlock_irqrestore(&pfa.lock, flags);
}

// 简化接口
pfn_t pfa_alloc_page(void) { return pfa_alloc(0); }
void  pfa_free_page(pfn_t pfn) { pfa_free(pfn, 0); }

// 引用计数 +1
void frame_get(pfn_t pfn) {
    if (!pfn_valid(pfn)) return;
    uint64_t flags = spin_lock_irqsave(&pfa.lock);
    pfa.meta[pfn].refcount++;
    spin_unlock_irqrestore(&pfa.lock, flags);
}

void frame_put(pfn_t pfn) {
    if (!pfn_valid(pfn)) return;
    uint64_t flags = spin_lock_irqsave(&pfa.lock);
    if (pfa.meta[pfn].refcount > 0 && --pfa.meta[pfn].refcount == 0) {
        int order = (int)pfa.meta[pfn].order;
        spin_unlock_irqrestore(&pfa.lock, flags);
        pfa_free(pfn, order);
        return;
    }
    spin_unlock_irqrestore(&pfa.lock, flags);
}

// 查询空闲页数量
size_t pfa_free_count(void) { return pfa.free_frames; }

void pfa_get_huge_stats(pfa_huge_stats_t *stats) {
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    if (PMD_ORDER > MAX_ORDER)
        return;

    uint64_t flags = spin_lock_irqsave(&pfa.lock);
    stats->total_huge_pages = pfa.total_frames / PMD_PAGE_COUNT;
    for (int order = PMD_ORDER; order <= MAX_ORDER; order++) {
        size_t huge_per_block = 1UL << (order - PMD_ORDER);
        stats->free_huge_pages += pfa.free_lists[order].count * huge_per_block;
    }
    spin_unlock_irqrestore(&pfa.lock, flags);
}

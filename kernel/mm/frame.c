#include "mm/frame.h"
#include "core/lock.h"
#include "core/panic.h"
#include "core/string.h"
#include "core/stdio.h"
#include "mm/oom.h"
struct task_t;
extern struct task_t *proc_current(void);
extern int proc_task_pid(const void *task);
#include "core/arch.h"
#include "core/timer.h"

/* Release trace (diagnostics): last N frame releases with caller info. */
#define FRAME_TRACE_ENTRIES 128
struct frame_trace_ent {
    uint64_t tick;
    pfn_t    pfn;
    uintptr_t ra;
    int      pid;
};
static struct frame_trace_ent g_frame_trace[FRAME_TRACE_ENTRIES];
static uint32_t g_frame_trace_idx;

static void frame_trace(pfn_t pfn)
{
    uint32_t i = __atomic_fetch_add(&g_frame_trace_idx, 1, __ATOMIC_RELAXED) %
                 FRAME_TRACE_ENTRIES;
    g_frame_trace[i].tick = timer_get_ticks();
    g_frame_trace[i].pfn  = pfn;
    g_frame_trace[i].ra   = (uintptr_t)__builtin_return_address(2);
    g_frame_trace[i].pid  = proc_current() ? proc_task_pid(proc_current()) : -1;
}

void frame_trace_dump_pfn(pfn_t pfn)
{
    printf("[FRAME-TRACE] releases of pfn=%lu:\n", (unsigned long)pfn);
    uint32_t n = g_frame_trace_idx;
    uint32_t start = n > FRAME_TRACE_ENTRIES ? n - FRAME_TRACE_ENTRIES : 0;
    for (uint32_t i = start; i < n; i++) {
        struct frame_trace_ent *e = &g_frame_trace[i % FRAME_TRACE_ENTRIES];
        if (e->pfn != pfn)
            continue;
        printf("  tick=%lu ra=0x%lx pid=%d\n",
               (unsigned long)e->tick, (unsigned long)e->ra, e->pid);
    }
}


pfa_t pfa;

int __popcountdi2(unsigned long long a) {
    int count = 0;
    while (a) {
        a &= (a - 1); // 每次清除最低位的 1
        count++;
    }
    return count;
}

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
    if (m->prev != PFN_NONE || m->next != PFN_NONE) {
        printf("[PFA DOUBLE-PUSH] pfn=%lu order=%d prev=%lu next=%lu flags=0x%x refcount=%u cpu=%u\n",
               (unsigned long)pfn, order,
               (unsigned long)m->prev, (unsigned long)m->next,
               m->flags, m->refcount, cpu_current_id());
        panic("pfa: duplicate free-list push");
    }
    m->prev = PFN_NONE;
    m->next = pfa.free_lists[order].head;
    if (m->next != PFN_NONE)
        meta_of(m->next)->prev = pfn;
    pfa.free_lists[order].head = pfn;
    pfa.free_lists[order].count++;
}

/*
 * Never publish a buddy block that still contains independently referenced
 * frames.  Huge-page demotion/COW and other split ownership paths can leave
 * live interior PFNs after the block head reaches zero references.  Putting
 * the whole block on a free list would let a later allocation overwrite those
 * live pages.  Split recursively and publish only completely unused pieces.
 */
static void fl_push_clean(pfn_t pfn, int order)
{
    if (order == 0) {
        if (pfa.meta[pfn].refcount > 0 ||
            pfa.meta[pfn].flags == FRAME_F_ALLOC)
            return;
        pfa.meta[pfn].flags = FRAME_F_FREE;
        pfa.meta[pfn].refcount = 0;
        pfa.meta[pfn].order = 0;
        pfa.meta[pfn].prev = PFN_NONE;
        pfa.meta[pfn].next = PFN_NONE;
        fl_push(pfn, 0);
        return;
    }

    int used = 0;
    for (pfn_t i = pfn; i < pfn + (1u << order); i++) {
        if (pfa.meta[i].refcount > 0) {
            used = 1;
            break;
        }
    }
    if (!used) {
        pfa.meta[pfn].flags = FRAME_F_FREE;
        pfa.meta[pfn].refcount = 0;
        pfa.meta[pfn].order = (uint8_t)order;
        pfa.meta[pfn].prev = PFN_NONE;
        pfa.meta[pfn].next = PFN_NONE;
        fl_push(pfn, order);
        return;
    }

    pfn_t half = 1u << (order - 1);
    fl_push_clean(pfn, order - 1);
    fl_push_clean(pfn + half, order - 1);
}

static void fl_remove(pfn_t pfn, int order) {
    frame_meta_t *m = meta_of(pfn);
    if (m->prev != PFN_NONE) {
        if (!pfn_valid(m->prev) || pfa.meta[m->prev].order != (uint8_t)order ||
            pfa.meta[m->prev].flags != FRAME_F_FREE) {
            panic("pfa: corrupted prev link");
        }
        meta_of(m->prev)->next = m->next;
    }
    else
        pfa.free_lists[order].head = m->next;
    if (m->next != PFN_NONE) {
        if (!pfn_valid(m->next) || pfa.meta[m->next].order != (uint8_t)order ||
            pfa.meta[m->next].flags != FRAME_F_FREE) {
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
    printf("[PFA] meta=%p total_frames=%lu meta_sz=%lu\n",
           (void *)meta, (unsigned long)pfa.total_frames,
           (unsigned long)meta_sz);
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

    if (pfa.meta[blk].flags != FRAME_F_FREE || pfa.meta[blk].order != (uint8_t)o ||
        (blk & ((1u << o) - 1)) != 0) {
        printf("[PFA DOUBLE-ALLOC] pfn=%lu flags=0x%x refcount=%u order_meta=%d list_order=%d free_lists[%d].count=%u cpu=%u\n",
               (unsigned long)blk, pfa.meta[blk].flags, pfa.meta[blk].refcount,
               (int)pfa.meta[blk].order, o, o, pfa.free_lists[o].count, cpu_current_id());
        panic("pfa: double allocation detected");
    }

    while (o > order) {
        o--;
        pfn_t buddy = blk ^ (1u << o);
        /* The buddy half must be entirely free; if any interior frame is
         * still allocated the block was corrupt when it entered the list. */
        for (pfn_t i = buddy; i < buddy + (1u << o); i++) {
            if (pfa.meta[i].flags == FRAME_F_ALLOC ||
                pfa.meta[i].refcount > 0) {
                printf("[PFA DIRTY-SPLIT] blk=%lu order=%d buddy=%lu "
                       "sub=%lu flags=0x%x refcount=%u order_meta=%d cpu=%u\n",
                       (unsigned long)blk, o, (unsigned long)buddy,
                       (unsigned long)i, pfa.meta[i].flags,
                       pfa.meta[i].refcount, (int)pfa.meta[i].order,
                       cpu_current_id());
                panic("pfa: dirty split block");
            }
        }
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

    for (pfn_t i = blk + 1; i < blk + (1u << order); i++) {
        if (pfa.meta[i].refcount > 0) continue;
        pfa.meta[i].flags    = FRAME_F_ALLOC;
        pfa.meta[i].refcount = 0;
        pfa.meta[i].order    = 0;
        pfa.meta[i].prev     = PFN_NONE;
        pfa.meta[i].next     = PFN_NONE;
    }

    pfa.free_frames -= (1u << order);
    return blk;
}

pfn_t pfa_alloc_flags(int order, int can_reclaim) {
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
            if (can_reclaim && oom_try_reclaim()) {
                retries++;
                continue;
            }
        }
        break;
    }

    return PFN_NONE;
}

pfn_t pfa_alloc(int order) { return pfa_alloc_flags(order, 1); }

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
        int buddy_dirty = 0;
        for (pfn_t i = buddy + 1; i < buddy + (1u << actual_order); i++) {
            if (i >= pfa.total_frames) break;
            if (pfa.meta[i].refcount > 0) { buddy_dirty = 1; break; }
        }
        if (buddy_dirty) break;

        fl_remove(buddy, actual_order);
        pfn = (pfn < buddy) ? pfn : buddy;
        actual_order++;
    }

    pfa.meta[pfn].flags = FRAME_F_FREE;
    pfa.meta[pfn].refcount = 0;
    pfa.meta[pfn].order = (uint8_t)actual_order;
    pfa.meta[pfn].prev = PFN_NONE;
    pfa.meta[pfn].next = PFN_NONE;

    /* 释放多页块时，清除子页的元数据，使 buddy merge 能正确合并。
     * 保护性检查：跳过仍在使用的页面（如内核栈），防止 buddy merge
     * 后的清除循环覆盖独立分配的页面元数据。 */
    for (pfn_t i = pfn + 1; i < pfn + (1u << actual_order); i++) {
        if (i >= pfa.total_frames) break;
        if (pfa.meta[i].refcount > 0) continue;
        pfa.meta[i].flags    = FRAME_F_FREE;
        pfa.meta[i].refcount = 0;
        pfa.meta[i].order    = 0;
        pfa.meta[i].prev     = PFN_NONE;
        pfa.meta[i].next     = PFN_NONE;
    }

    fl_push_clean(pfn, actual_order);
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
        frame_trace(pfn);
        /* refcount 归零，直接在此释放（内联 pfa_free 核心逻辑），
         * 避免先解锁再调 pfa_free 的竞态窗口 */
        int actual_order = (int)pfa.meta[pfn].order;
        if (actual_order < 0 || actual_order > MAX_ORDER) {
            spin_unlock_irqrestore(&pfa.lock, flags);
            return;
        }
        pfa.free_frames += (1u << actual_order);
        const pfa_range_t *range = pfa_range_for_pfn(pfn);
        if (!range) {
            spin_unlock_irqrestore(&pfa.lock, flags);
            return;
        }
        while (actual_order < MAX_ORDER) {
            pfn_t buddy = pfn ^ (1u << actual_order);
            if (buddy >= pfa.total_frames) break;
            if (buddy < range->start_pfn || buddy >= range->end_pfn) break;
            if (pfa.meta[buddy].flags != FRAME_F_FREE) break;
            if (pfa.meta[buddy].order != (uint8_t)actual_order) break;
            int buddy_dirty = 0;
            for (pfn_t i = buddy + 1; i < buddy + (1u << actual_order); i++) {
                if (i >= pfa.total_frames) break;
                if (pfa.meta[i].refcount > 0) { buddy_dirty = 1; break; }
            }
            if (buddy_dirty) break;
            fl_remove(buddy, actual_order);
            pfn = (pfn < buddy) ? pfn : buddy;
            actual_order++;
        }
        pfa.meta[pfn].flags    = FRAME_F_FREE;
        pfa.meta[pfn].refcount = 0;
        pfa.meta[pfn].order    = (uint8_t)actual_order;
        pfa.meta[pfn].prev     = PFN_NONE;
        pfa.meta[pfn].next     = PFN_NONE;
        for (pfn_t i = pfn + 1; i < pfn + (1u << actual_order); i++) {
            if (i >= pfa.total_frames) break;
            if (pfa.meta[i].refcount > 0) continue;
            pfa.meta[i].flags    = FRAME_F_FREE;
            pfa.meta[i].refcount = 0;
            pfa.meta[i].order    = 0;
            pfa.meta[i].prev     = PFN_NONE;
            pfa.meta[i].next     = PFN_NONE;
        }
        fl_push_clean(pfn, actual_order);
        spin_unlock_irqrestore(&pfa.lock, flags);
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

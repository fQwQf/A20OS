#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/oom.h"
#include "core/cpu.h"
#include "core/lock.h"
#include "core/lock_counters.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/stdio.h"

#define SLAB_NR_CACHES  7
#define SLAB_MAX_OBJ   2048  // Largest object size; anything larger goes straight to the buddy allocator
#define SLAB_HDR_SIZE   64
#define SLAB_MAGIC   0x534C4142U  // "SLAB"
#define BIG_MAGIC    0x42494741U  // "BIGA"
#define SLAB_SPARE_CAP  2
#define SLAB_BITMAP_WORDS  2
#define SLAB_BITMAP_BITS   (SLAB_BITMAP_WORDS * 64)

#ifndef CONFIG_SLAB_DEBUG
#define CONFIG_SLAB_DEBUG 0
#endif

// Slab caches of increasing size, from 32 to 2048 bytes
static const size_t slab_sizes[SLAB_NR_CACHES] = {
    32, 64, 128, 256, 512, 1024, 2048
};

typedef struct slab_page {
    struct slab_page *next;
    struct slab_page *prev;
    uint16_t in_use;
    uint16_t total;
    void    *free_list;
    uint64_t alloc_bits[SLAB_BITMAP_WORDS]; /* bitmap of currently allocated objects */
    uint8_t  cache_idx;
    uint8_t  state;          // SLAB_STATE_*
    uint8_t  _pad[2];
    uint32_t magic;
} slab_page_t;

enum {
    SLAB_STATE_NONE = 0,
    SLAB_STATE_PARTIAL = 1,
    SLAB_STATE_FULL = 2,
    SLAB_STATE_SPARE = 3,
};

typedef struct big_alloc_hdr {
    uint32_t magic;
    uint16_t order;
    uint16_t _pad;
    uint64_t reserved;
} big_alloc_hdr_t;

_Static_assert(sizeof(big_alloc_hdr_t) == 16,
               "large kmalloc results must retain 16-byte alignment");

#define BIG_CANARY 0xCAFEBABEUL

static int big_alloc_canary_ok(const big_alloc_hdr_t *hdr)
{
    uint64_t *canary = (uint64_t *)((const uint8_t *)(hdr + 1) +
                                    (((size_t)1 << hdr->order) * PAGE_SIZE -
                                     sizeof(big_alloc_hdr_t) - 8));
    return *canary == BIG_CANARY;
}

typedef struct {
    size_t         obj_size;
    size_t         objs_per_slab;
    slab_page_t   *partial;
    slab_page_t   *full;
    slab_page_t   *spare;
    size_t         spare_count;
    spinlock_t     lock;
} slab_cache_t;

static slab_cache_t caches[SLAB_NR_CACHES];

/* Per-CPU object arrays, mirroring the frame allocator's CPU page batch:
 * a hit satisfies kmalloc/kfree with only local IRQ exclusion, and the
 * cache lock is taken once per refill/drain instead of once per object.
 * Hoarding is bounded: SLAB_CPU_ARRAY_CAP objects per (cache, cpu). */
#define SLAB_CPU_ARRAY_CAP 16
#define SLAB_CPU_REFILL    8
typedef struct {
    void    *objs[SLAB_CPU_ARRAY_CAP];
    uint16_t count;
    uint16_t _pad;
} __attribute__((aligned(64))) slab_cpu_array_t;

static slab_cpu_array_t g_slab_cpu[SLAB_NR_CACHES][CONFIG_NR_CPUS];

static inline slab_cpu_array_t *slab_cpu_array(int idx)
{
    unsigned cpu = arch_current_cpu_id();
    if (cpu >= CONFIG_NR_CPUS)
        cpu = 0;
    return &g_slab_cpu[idx][cpu];
}

static int slab_popcount64(uint64_t bits) {
    return __builtin_popcountll(bits);
}

static int slab_popcount(const slab_page_t *sp) {
    int n = 0;
    for (int i = 0; i < SLAB_BITMAP_WORDS; i++)
        n += slab_popcount64(sp->alloc_bits[i]);
    return n;
}

static inline uint64_t slab_bit_mask(uint16_t obj_idx) {
    return 1ULL << (obj_idx & 63);
}

static inline int slab_bit_test(const slab_page_t *sp, uint16_t obj_idx) {
    return (sp->alloc_bits[obj_idx >> 6] & slab_bit_mask(obj_idx)) != 0;
}

static inline void slab_bit_set(slab_page_t *sp, uint16_t obj_idx) {
    sp->alloc_bits[obj_idx >> 6] |= slab_bit_mask(obj_idx);
}

static inline void slab_bit_clear(slab_page_t *sp, uint16_t obj_idx) {
    sp->alloc_bits[obj_idx >> 6] &= ~slab_bit_mask(obj_idx);
}

static int slab_page_valid(slab_page_t *sp) {
    if (!sp) return 0;
    if (sp->magic != SLAB_MAGIC) return 0;
    if (sp->cache_idx >= SLAB_NR_CACHES) return 0;
    if (sp->state != SLAB_STATE_PARTIAL &&
        sp->state != SLAB_STATE_FULL &&
        sp->state != SLAB_STATE_SPARE)
        return 0;
    if (sp->total != caches[sp->cache_idx].objs_per_slab) return 0;
    if (sp->total > SLAB_BITMAP_BITS) return 0;
    if (CONFIG_SLAB_DEBUG && slab_popcount(sp) != sp->in_use) return 0;
    return 1;
}

void slab_init(void) {
    if (sizeof(slab_page_t) > SLAB_HDR_SIZE)
        panic("slab_init: slab header larger than SLAB_HDR_SIZE");
    for (int i = 0; i < SLAB_NR_CACHES; i++) {
        caches[i].obj_size     = slab_sizes[i];
        caches[i].objs_per_slab = (PAGE_SIZE - SLAB_HDR_SIZE) / slab_sizes[i];
        if (caches[i].objs_per_slab > SLAB_BITMAP_BITS)
            panic("slab_init: slab bitmap too small for cache");
        caches[i].partial = NULL;
        caches[i].full    = NULL;
        caches[i].spare   = NULL;
        caches[i].spare_count = 0;
        spin_init(&caches[i].lock);
        /* Publish the cache lock to /proc/a20/lock_contention.  spin_lock_at()
         * already accumulates contended_acquires/spins unconditionally, so
         * registration only makes the numbers reachable -- it costs the fast
         * path nothing.  Without it the per-CPU arrays leave no trace at all
         * and the before/after comparison has no slab dimension to read. */
        lock_counters_register(&caches[i].lock, "slab_cache");
    }
}

// Allocate and initialise a new Slab page for the given cache
static slab_page_t *slab_grow(int idx) {
    slab_cache_t *c = &caches[idx];
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) return NULL;

    slab_page_t *sp = (slab_page_t *)pfn_to_virt(pfn);
    memset(sp, 0, PAGE_SIZE);
    sp->cache_idx = (uint8_t)idx;
    sp->in_use    = 0;
    sp->total     = (uint16_t)c->objs_per_slab;
    sp->prev      = NULL;
    sp->next      = NULL;
    memset(sp->alloc_bits, 0, sizeof(sp->alloc_bits));
    sp->state     = SLAB_STATE_NONE;
    sp->magic     = SLAB_MAGIC;

    // If the object is so large that not even one fits in a page, give up
    if (sp->total == 0) {
        pfa_free_page(pfn);
        return NULL;
    }

    char *obj = (char *)sp + SLAB_HDR_SIZE;
    sp->free_list = obj;
    for (uint16_t i = 0; i < sp->total - 1; i++) {
        void **slot = (void **)obj;
        *slot = obj + c->obj_size;
        obj  += c->obj_size;
    }
    *(void **)obj = NULL;
    return sp;
}

static slab_page_t *slab_spare_pop(slab_cache_t *c) {
    slab_page_t *sp = c->spare;
    if (!sp) return NULL;
    c->spare = sp->next;
    if (c->spare) c->spare->prev = NULL;
    sp->prev = sp->next = NULL;
    if (c->spare_count > 0) c->spare_count--;
    sp->state = SLAB_STATE_NONE;
    return sp;
}

static void slab_spare_push(slab_cache_t *c, slab_page_t *sp) {
    sp->prev = NULL;
    sp->next = c->spare;
    if (c->spare) c->spare->prev = sp;
    c->spare = sp;
    c->spare_count++;
    sp->state = SLAB_STATE_SPARE;
}

static void slab_page_release(slab_page_t *sp) {
    pfn_t pfn = virt_to_pfn(sp);
    sp->magic = 0;
    sp->state = SLAB_STATE_NONE;
    sp->free_list = NULL;
    sp->next = NULL;
    sp->prev = NULL;
    memset(sp->alloc_bits, 0, sizeof(sp->alloc_bits));
    if (pfn_valid(pfn)) pfa_free_page(pfn);
}

static void slab_list_remove(slab_page_t **head, slab_page_t *sp) {
    if (sp->prev) sp->prev->next = sp->next;
    else *head = sp->next;
    if (sp->next) sp->next->prev = sp->prev;
    sp->prev = sp->next = NULL;
}

static void slab_list_push(slab_page_t **head, slab_page_t *sp) {
    sp->prev = NULL;
    sp->next = *head;
    if (*head) (*head)->prev = sp;
    *head = sp;
}

static __attribute__((unused)) int slab_list_contains(slab_page_t *head, slab_page_t *sp) {
    for (slab_page_t *p = head; p; p = p->next) {
        if (p == sp) return 1;
    }
    return 0;
}

// Verify the integrity of a Slab page (debug use)
static __attribute__((unused)) void slab_validate_sp(slab_page_t *sp, const char *where, size_t obj_size) {
    int free_count = 0;
    for (void *p = sp->free_list; p; p = *(void **)p) {
        uintptr_t offset = (uintptr_t)p - (uintptr_t)sp;
        if ((offset - SLAB_HDR_SIZE) % obj_size != 0 || offset >= PAGE_SIZE) {
            printf("[SLAB BUG] %s: corrupted free_list node=%p sp=%p offset=%lu obj_size=%lu\n",
                   where, p, (void *)sp, (unsigned long)offset, (unsigned long)obj_size);
            uint64_t *page = (uint64_t *)sp;
            printf("[SLAB DBG] sp page hex dump:\n");
            for (int i = 0; i < 16; i++) {
                printf("  [%d] %016lx\n", i, (unsigned long)page[i]);
            }
            panic("slab_validate_sp: corrupted free_list");
        }
        free_count++;
        if (free_count > sp->total) {
            printf("[SLAB BUG] %s: free_list cycle or overflow sp=%p free_count=%d total=%u\n",
                   where, (void *)sp, free_count, (unsigned)sp->total);
            panic("slab_validate_sp: free_list cycle");
        }
    }
    if ((int)(sp->in_use + free_count) != sp->total) {
        printf("[SLAB BUG] %s: in_use/free mismatch sp=%p in_use=%u free=%d total=%u\n",
               where, (void *)sp, (unsigned)sp->in_use, free_count, (unsigned)sp->total);
        panic("slab_validate_sp: count mismatch");
    }
}

/* Allocate one object from the cache's partial/space lists.  Caller holds
 * c->lock.  Returns NULL only when a new slab page cannot be grown. */
static void *slab_alloc_obj_locked(int idx) {
    slab_cache_t *c = &caches[idx];
    slab_page_t *sp = c->partial;

    /* Self-heal stale partial list entries that are already full. */
    while (sp && sp->free_list == NULL) {
        slab_list_remove(&c->partial, sp);
        slab_list_push(&c->full, sp);
        sp = c->partial;
    }

    if (!sp) {
        sp = slab_spare_pop(c);
        if (!sp) {
            sp = slab_grow(idx);
            if (!sp)
                return NULL;
        }
        sp->state = SLAB_STATE_PARTIAL;
        slab_list_push(&c->partial, sp);
    }

    size_t obj_size = c->obj_size;
    if (!slab_page_valid(sp)) {
        printf("[SLAB BUG] kmalloc: invalid slab page sp=%p idx=%d magic=0x%x cache_idx=%u total=%u\n",
               (void *)sp, idx, sp ? sp->magic : 0U, sp ? sp->cache_idx : 0U,
               sp ? (unsigned)sp->total : 0U);
        panic("kmalloc: invalid slab page");
    }
#if CONFIG_SLAB_DEBUG
    slab_validate_sp(sp, "kmalloc-pre", obj_size);
#endif

    void *obj = sp->free_list;
    if (!obj) {
        printf("[SLAB BUG] kmalloc: free_list is NULL but in_use=%u/%u sp=%p idx=%d\n",
               sp->in_use, sp->total, (void *)sp, idx);
        panic("kmalloc: empty free_list");
    }
    /* Validate that obj lies inside this slab page */
    uintptr_t offset = (uintptr_t)obj - (uintptr_t)sp;
    if ((offset - SLAB_HDR_SIZE) % obj_size != 0 || offset >= PAGE_SIZE) {
        printf("[SLAB BUG] kmalloc: bad free_list obj=%p sp=%p offset=%lu idx=%d\n",
               obj, (void *)sp, (unsigned long)offset, idx);
        printf("[SLAB DBG] hex dump of sp page:\n");
        uint64_t *p = (uint64_t *)sp;
        for (int i = 0; i < 16; i++) printf("  [%d] %016lx\n", i, (unsigned long)p[i]);
        panic("kmalloc: corrupted free_list");
    }
    uint16_t obj_idx = (uint16_t)((offset - SLAB_HDR_SIZE) / obj_size);
    if (slab_bit_test(sp, obj_idx)) {
        printf("[SLAB BUG] kmalloc: object already allocated sp=%p obj=%p obj_idx=%u in_use=%u total=%u\n",
               (void *)sp, obj, (unsigned)obj_idx, sp->in_use, sp->total);
        panic("kmalloc: alloc_bits corrupted");
    }
    sp->free_list = *(void **)obj;
    slab_bit_set(sp, obj_idx);
    sp->in_use++;

    if (sp->in_use == sp->total) {
        slab_list_remove(&c->partial, sp);
        sp->state = SLAB_STATE_FULL;
        slab_list_push(&c->full, sp);
    }

    return obj;
}

/* Free one object back to its slab.  Caller holds the cache lock named by
 * the slab's own cache_idx (validated here).  Carries the same corruption
 * diagnostics as the historic kfree body. */
static void slab_free_obj_locked(void *ptr, uintptr_t caller_ra) {
    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)sp;
    int idx = sp->cache_idx;
    slab_cache_t *c = &caches[idx];

    if (!slab_page_valid(sp) || sp->cache_idx != idx) {
        uint8_t actual_idx = sp->cache_idx;
        printf("[SLAB BUG] kfree(%p): stale slab page sp=%p cache_idx=%u expected=%d ra=0x%lx\n",
               ptr, (void *)sp, actual_idx, idx, (unsigned long)caller_ra);
        panic("kfree: stale slab page");
    }
    if (offset < SLAB_HDR_SIZE) {
        uint8_t cache_idx = sp->cache_idx;
        printf("[SLAB BUG] kfree(%p): pointer points into slab header sp=%p offset=%lu cache_idx=%u ra=0x%lx\n",
               ptr, (void *)sp, (unsigned long)offset, cache_idx,
               (unsigned long)caller_ra);
        panic("kfree: pointer inside slab header");
    }

    /* sanity check: ptr must be aligned to obj_size and within the page */
    size_t obj_size = slab_sizes[idx];
    if ((offset - SLAB_HDR_SIZE) % obj_size != 0 || offset >= PAGE_SIZE) {
        uint8_t cache_idx = sp->cache_idx;
        printf("[SLAB BUG] kfree(%p) bad offset=%lu sp=%p cache_idx=%u obj_size=%lu ra=0x%lx\n",
               ptr, (unsigned long)offset, (void *)sp, cache_idx,
               (unsigned long)obj_size, (unsigned long)caller_ra);
        panic("kfree: corrupted slab pointer");
    }
    uint16_t obj_idx = (uint16_t)((offset - SLAB_HDR_SIZE) / obj_size);
    if (!slab_bit_test(sp, obj_idx)) {
        uint8_t cache_idx = sp->cache_idx;
        uint8_t state = sp->state;
        printf("[SLAB BUG] kfree(%p): object not allocated sp=%p cache_idx=%u obj_idx=%u state=%u ra=0x%lx\n",
               ptr, (void *)sp, cache_idx, (unsigned)obj_idx,
               (unsigned)state, (unsigned long)caller_ra);
        panic("kfree: stale or double free");
    }

#if CONFIG_SLAB_DEBUG
    slab_validate_sp(sp, "kfree-pre", obj_size);
#endif

    // Return the object to the free list.  Must be done under the lock, or a
    // concurrent kfree can corrupt the list.
    slab_bit_clear(sp, obj_idx);
    *(void **)ptr = sp->free_list;
    sp->free_list = ptr;
    sp->in_use--;

#if CONFIG_SLAB_DEBUG
    slab_validate_sp(sp, "kfree-post", obj_size);
#endif

    // The page has just left the full state: if decrementing leaves
    // in_use == total - 1, it must have been on the full list beforehand
    if (sp->in_use == sp->total - 1) {
        slab_list_remove(&c->full, sp);
        sp->state = SLAB_STATE_PARTIAL;
        slab_list_push(&c->partial, sp);
    }

    // The page is now completely free.  Note this is an if and not an else if,
    // which handles the edge case of total == 1
    if (sp->in_use == 0) {
        if (sp->state == SLAB_STATE_PARTIAL) {
            slab_list_remove(&c->partial, sp);
        } else if (sp->state == SLAB_STATE_FULL) {
            slab_list_remove(&c->full, sp);
        }
        if (c->spare_count < SLAB_SPARE_CAP) {
            slab_spare_push(c, sp);
        } else {
            slab_page_release(sp);
        }
    }
}

void *kmalloc_flags(size_t size, int can_reclaim) {
    if (size == 0) return NULL;

    if (size >= SLAB_MAX_OBJ) {
        int order = 0;
        size_t need = ROUND_UP(size + sizeof(big_alloc_hdr_t) + 8, PAGE_SIZE);
        while ((1u << order) * PAGE_SIZE < need) order++;
        if (order > MAX_ORDER) return NULL;
        pfn_t pfn = pfa_alloc_flags(order, can_reclaim);
        if (pfn == PFN_NONE)
            return NULL;
        big_alloc_hdr_t *hdr = (big_alloc_hdr_t *)pfn_to_virt(pfn);
        hdr->magic = BIG_MAGIC;
        hdr->order = (uint16_t)order;
        hdr->_pad  = 0;
        hdr->reserved = 0;
        uint64_t *canary = (uint64_t *)((uint8_t *)(hdr + 1) +
                                        ((size_t)1 << order) * PAGE_SIZE -
                                        sizeof(big_alloc_hdr_t) - 8);
        *canary = BIG_CANARY;
        return (void *)(hdr + 1);
    }

    int idx = 0;
    while (idx < SLAB_NR_CACHES - 1 && slab_sizes[idx] < size) idx++;

    /* CPU-array fast path: identical discipline to the frame allocator's
     * order-0 batch — local IRQ exclusion protects the array against a
     * re-entrant interrupt handler and against migration mid-pop.  Sample the
     * IRQ flags and mask first, then resolve the CPU: reading the CPU id before
     * masking is frame.c's discipline reversed, and it would leave the array
     * pointer pinned to a CPU this thread could still leave. */
    uint64_t irq_gate = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    slab_cpu_array_t *arr = slab_cpu_array(idx);
    if (arr->count) {
        void *obj = arr->objs[--arr->count];
        if (irq_gate)
            arch_local_irq_enable();
        return obj;
    }
    if (irq_gate)
        arch_local_irq_enable();

    slab_cache_t *c = &caches[idx];
    uint64_t irq_flags = spin_lock_irqsave(&c->lock);
    void *obj = slab_alloc_obj_locked(idx);
    if (obj) {
        /* Refill the local array while the lock is already held, so the
         * next SLAB_CPU_REFILL-1 allocations skip it entirely. */
        while (arr->count < SLAB_CPU_REFILL) {
            void *extra = slab_alloc_obj_locked(idx);
            if (!extra)
                break;
            arr->objs[arr->count++] = extra;
        }
    }
    spin_unlock_irqrestore(&c->lock, irq_flags);

    if (!obj && can_reclaim) {
        oom_try_reclaim();
        return kmalloc_flags(size, 0);
    }
    return obj;
}

void kfree(void *ptr) {
    if (!ptr) return;
    uint64_t caller_ra = arch_read_ra();

    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));

    big_alloc_hdr_t *bhdr = (big_alloc_hdr_t *)ptr - 1;
    if (bhdr->magic == BIG_MAGIC && bhdr->order <= MAX_ORDER) {
        int order = (int)bhdr->order;
        pfn_t pfn = virt_to_pfn(bhdr);
        if (pfn_valid(pfn) &&
            pfa.meta[pfn].flags == FRAME_F_ALLOC &&
            pfa.meta[pfn].refcount > 0 &&
            (uintptr_t)bhdr % PAGE_SIZE == 0) {
            if (!big_alloc_canary_ok(bhdr)) {
                printf("[SLAB BUG] kfree(%p): big-alloc canary clobbered order=%u ra=0x%lx\n",
                       ptr, (unsigned)order, (unsigned long)caller_ra);
                extern void a20_channel_trace_dump(void);
                a20_channel_trace_dump();
                panic("kfree: big-alloc canary overwritten");
            }
            pfa_free(pfn, order);
            return;
        }
    }

    int is_slab = slab_page_valid(sp);

    if (!is_slab) {
        pfn_t dbg_pfn = virt_to_pfn(bhdr);
        printf("[SLAB BUG] kfree(%p): invalid non-slab pointer hdr=%p magic=0x%x order=%u ra=0x%lx\n",
               ptr, (void *)bhdr, bhdr->magic, bhdr->order, (unsigned long)caller_ra);
        if (pfn_valid(dbg_pfn))
        printf("[SLAB BUG]   pfn=%lu flags=0x%x refcount=%u order_meta=%d cpu=%u\n",
               (unsigned long)dbg_pfn, pfa.meta[dbg_pfn].flags,
               pfa.meta[dbg_pfn].refcount, pfa.meta[dbg_pfn].order,
               cpu_current_id());
        else
            printf("[SLAB BUG]   pfn=%lu invalid cpu=%u\n",
                   (unsigned long)dbg_pfn, cpu_current_id());
        extern void a20_channel_trace_dump(void);
        extern void frame_trace_dump_pfn(pfn_t pfn);
        extern uint32_t rv64_tlb_flush_stats(uint32_t *serviced)
            __attribute__((weak));
        uint32_t tlb_svc = 0;
        uint32_t tlb_calls = rv64_tlb_flush_stats
            ? rv64_tlb_flush_stats(&tlb_svc) : 0;
        printf("[SLAB BUG]   tlb_flush_calls=%u tlb_ipi_serviced=%u\n",
               (unsigned)tlb_calls, (unsigned)tlb_svc);
        a20_channel_trace_dump();
        frame_trace_dump_pfn(dbg_pfn);
        panic("kfree: invalid pointer");
    }

    int idx = sp->cache_idx;
    uint64_t irq_gate = arch_irqs_enabled() ? 1 : 0;
    arch_local_irq_disable();
    slab_cpu_array_t *arr = slab_cpu_array(idx);
    if (arr->count < SLAB_CPU_ARRAY_CAP) {
        arr->objs[arr->count++] = ptr;
        if (irq_gate)
            arch_local_irq_enable();
        return;
    }
    if (irq_gate)
        arch_local_irq_enable();

    /* Array full: drain half back to their slabs under the lock, then park
     * this object in the array. */
    slab_cache_t *c = &caches[idx];
    uint64_t irq_flags = spin_lock_irqsave(&c->lock);
    for (uint16_t i = 0; i < SLAB_CPU_ARRAY_CAP / 2; i++)
        slab_free_obj_locked(arr->objs[--arr->count], caller_ra);
    arr->objs[arr->count++] = ptr;
    spin_unlock_irqrestore(&c->lock, irq_flags);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    size_t old_size;
    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)sp;
    if (!slab_page_valid(sp)) {
        big_alloc_hdr_t *hdr = (big_alloc_hdr_t *)ptr - 1;
        if (hdr->magic != BIG_MAGIC || hdr->order > MAX_ORDER)
            panic("krealloc: invalid pointer");
        old_size = ((size_t)1 << (int)hdr->order) * PAGE_SIZE - sizeof(big_alloc_hdr_t);
    } else {
        if (offset < SLAB_HDR_SIZE)
            panic("krealloc: pointer inside slab header");
        old_size = slab_sizes[sp->cache_idx];
    }

    if (new_size <= old_size) return ptr;

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    return new_ptr;
}

void *kmalloc(size_t size) { return kmalloc_flags(size, 1); }
void *kmalloc_atomic(size_t size) { return kmalloc_flags(size, 0); }

void *kcalloc(size_t nmemb, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(nmemb, size, &total)) return NULL;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

void slab_get_stats(slab_stats_t *stats)
{
    if (!stats)
        return;
    memset(stats, 0, sizeof(*stats));
    for (int i = 0; i < SLAB_NR_CACHES; i++) {
        slab_cache_t *c = &caches[i];
        uint64_t flags = spin_lock_irqsave(&c->lock);
        for (slab_page_t *sp = c->partial; sp; sp = sp->next) {
            stats->total_pages++;
            stats->active_pages++;
            stats->allocated_objects += sp->in_use;
            stats->allocated_bytes += (size_t)sp->in_use * c->obj_size;
        }
        for (slab_page_t *sp = c->full; sp; sp = sp->next) {
            stats->total_pages++;
            stats->active_pages++;
            stats->allocated_objects += sp->in_use;
            stats->allocated_bytes += (size_t)sp->in_use * c->obj_size;
        }
        for (slab_page_t *sp = c->spare; sp; sp = sp->next) {
            stats->total_pages++;
            stats->spare_pages++;
        }
        spin_unlock_irqrestore(&c->lock, flags);
    }
    stats->total_bytes = stats->total_pages * PAGE_SIZE;
    stats->reclaimable_bytes = stats->spare_pages * PAGE_SIZE;
}

size_t slab_reclaim_spare(void)
{
    size_t freed = 0;
    for (int i = 0; i < SLAB_NR_CACHES; i++) {
        slab_cache_t *c = &caches[i];
        uint64_t flags = spin_lock_irqsave(&c->lock);
        while (c->spare) {
            slab_page_t *sp = c->spare;
            c->spare = sp->next;
            if (c->spare) c->spare->prev = NULL;
            c->spare_count--;
            slab_page_release(sp);
            freed++;
        }
        spin_unlock_irqrestore(&c->lock, flags);
    }
    return freed * PAGE_SIZE;
}

void *kcalloc_atomic(size_t nmemb, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(nmemb, size, &total)) return NULL;
    void *p = kmalloc_atomic(total);
    if (p) memset(p, 0, total);
    return p;
}

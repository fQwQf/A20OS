#include "mm/slab.h"
#include "mm/frame.h"
#include "mm/oom.h"
#include "core/lock.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/stdio.h"

#define SLAB_NR_CACHES  7  // Slab 缓存数量
#define SLAB_MAX_OBJ   2048  // 最大对象大小，超过此大小直接使用 buddy 分配器
#define SLAB_HDR_SIZE   64  // Slab 页面头部大小
#define SLAB_MAGIC   0x534C4142U  // "SLAB"
#define BIG_MAGIC    0x42494741U  // "BIGA"
#define SLAB_SPARE_CAP  8
#define SLAB_BITMAP_WORDS  2
#define SLAB_BITMAP_BITS   (SLAB_BITMAP_WORDS * 64)

#ifndef CONFIG_SLAB_DEBUG
#define CONFIG_SLAB_DEBUG 0
#endif

// 不同大小的 Slab 缓存（32 字节到 2048 字节）
static const size_t slab_sizes[SLAB_NR_CACHES] = {
    32, 64, 128, 256, 512, 1024, 2048
};

// Slab 页面结构，存储在每页的开头
typedef struct slab_page {
    struct slab_page *next;  // 链表下一页
    struct slab_page *prev;  // 链表前一页
    uint16_t in_use;         // 已使用的对象数量
    uint16_t total;          // 总对象数量
    void    *free_list;      // 空闲对象链表头
    uint64_t alloc_bits[SLAB_BITMAP_WORDS]; /* 当前已分配对象位图 */
    uint8_t  cache_idx;      // 所属缓存索引
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
} big_alloc_hdr_t;

// Slab 缓存结构
typedef struct {
    size_t         obj_size;
    size_t         objs_per_slab;
    slab_page_t   *partial;
    slab_page_t   *full;
    slab_page_t   *spare;
    size_t         spare_count;
    spinlock_t     lock;
} slab_cache_t;

// 全局 Slab 缓存数组
static slab_cache_t caches[SLAB_NR_CACHES];

static int slab_popcount64(uint64_t bits) {
    int n = 0;
    while (bits) {
        bits &= bits - 1;
        n++;
    }
    return n;
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

// Slab 分配器初始化
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
    }
}

// 为指定缓存分配一个新的 Slab 页面并初始化
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

    // 如果对象太大导致一页放不下任何一个，则放弃分配
    if (sp->total == 0) {
        pfa_free_page(pfn);
        return NULL;
    }

    // 初始化空闲对象链表
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

// 从链表中移除一个 Slab 页面
static void slab_list_remove(slab_page_t **head, slab_page_t *sp) {
    if (sp->prev) sp->prev->next = sp->next;
    else *head = sp->next;
    if (sp->next) sp->next->prev = sp->prev;
    sp->prev = sp->next = NULL;
}

// 将 Slab 页面添加到链表头部
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

// 验证 Slab 页面的完整性（调试用）
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

// 分配指定大小的内存（Slab 分配器）
void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    if (size >= SLAB_MAX_OBJ) {
        int order = 0;
        size_t need = ROUND_UP(size + sizeof(big_alloc_hdr_t), PAGE_SIZE);
        while ((1u << order) * PAGE_SIZE < need) order++;
        if (order > MAX_ORDER) return NULL;
        pfn_t pfn = pfa_alloc(order);
        if (pfn == PFN_NONE) {
            oom_try_reclaim();
            return NULL;
        }
        big_alloc_hdr_t *hdr = (big_alloc_hdr_t *)pfn_to_virt(pfn);
        hdr->magic = BIG_MAGIC;
        hdr->order = (uint16_t)order;
        hdr->_pad  = 0;
        return (void *)(hdr + 1);
    }

    // 选择合适的缓存大小
    int idx = 0;
    while (idx < SLAB_NR_CACHES - 1 && slab_sizes[idx] < size) idx++;

    slab_cache_t *c = &caches[idx];
    uint64_t irq_flags = spin_lock_irqsave(&c->lock);
    slab_page_t *sp = c->partial;

    /* Self-heal stale partial list entries that are already full. */
    while (sp && sp->free_list == NULL) {
        slab_list_remove(&c->partial, sp);
        slab_list_push(&c->full, sp);
        sp = c->partial;
    }

    // 如果没有部分使用的页面，使用备用页面或分配新页面
    if (!sp) {
        sp = slab_spare_pop(c);
        if (!sp) {
            sp = slab_grow(idx);
            if (!sp) {
                spin_unlock_irqrestore(&c->lock, irq_flags);
                oom_try_reclaim();
                return NULL;
            }
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
    // slab_validate_sp(sp, "kmalloc-pre", obj_size);

    // 从空闲链表取出一个对象
    void *obj = sp->free_list;
    if (!obj) {
        printf("[SLAB BUG] kmalloc: free_list is NULL but in_use=%u/%u sp=%p idx=%d\n",
               sp->in_use, sp->total, (void *)sp, idx);
        panic("kmalloc: empty free_list"); // 可能在这里出错
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

    // 如果页面已满，移动到 full 链表
    if (sp->in_use == sp->total) {
        slab_list_remove(&c->partial, sp);
        sp->state = SLAB_STATE_FULL;
        slab_list_push(&c->full, sp);
    }

    spin_unlock_irqrestore(&c->lock, irq_flags);
    return obj;
}

// 释放 Slab 分配的内存
void kfree(void *ptr) {
    if (!ptr) return;
    uint64_t caller_ra = arch_read_ra();

    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)sp;

    /* Check big alloc first: header sits at ptr - sizeof(big_alloc_hdr_t).
     * This must precede the slab check because a big-alloc page can have
     * incidental bytes at the slab-magic offset that falsely pass slab_page_valid. */
    big_alloc_hdr_t *bhdr = (big_alloc_hdr_t *)ptr - 1;
    if (bhdr->magic == BIG_MAGIC && bhdr->order <= MAX_ORDER) {
        int order = (int)bhdr->order;
        pfn_t pfn = virt_to_pfn(bhdr);
        if (pfn_valid(pfn)) pfa_free(pfn, order);
        return;
    }

    int is_slab = slab_page_valid(sp);

    if (!is_slab) {
        printf("[SLAB BUG] kfree(%p): invalid non-slab pointer hdr=%p magic=0x%x order=%u ra=0x%lx\n",
               ptr, (void *)bhdr, bhdr->magic, bhdr->order, (unsigned long)caller_ra);
        panic("kfree: invalid pointer");
    }

    if (offset < SLAB_HDR_SIZE) {
        printf("[SLAB BUG] kfree(%p): pointer points into slab header sp=%p offset=%lu cache_idx=%u ra=0x%lx\n",
               ptr, (void *)sp, (unsigned long)offset, sp->cache_idx, (unsigned long)caller_ra);
        panic("kfree: pointer inside slab header");
    }

    /* sanity check: ptr must be aligned to obj_size and within the page */
    size_t obj_size = slab_sizes[sp->cache_idx];
    if ((offset - SLAB_HDR_SIZE) % obj_size != 0 || offset >= PAGE_SIZE) {
        printf("[SLAB BUG] kfree(%p) bad offset=%lu sp=%p cache_idx=%u obj_size=%lu\n",
               ptr, (unsigned long)offset, (void *)sp, sp->cache_idx, (unsigned long)obj_size);
        panic("kfree: corrupted slab pointer");
    }
    uint16_t obj_idx = (uint16_t)((offset - SLAB_HDR_SIZE) / obj_size);
    if (!slab_bit_test(sp, obj_idx)) {
        printf("[SLAB BUG] kfree(%p): object not allocated sp=%p cache_idx=%u obj_idx=%u state=%u ra=0x%lx\n",
               ptr, (void *)sp, sp->cache_idx, (unsigned)obj_idx,
               (unsigned)sp->state, (unsigned long)caller_ra);
        panic("kfree: stale or double free");
    }

    // slab_validate_sp(sp, "kfree-pre", obj_size);

    int idx = sp->cache_idx;
    slab_cache_t *c = &caches[idx];
    uint64_t irq_flags = spin_lock_irqsave(&c->lock);

    // 将对象放回空闲链表（必须在锁内，防止并发 kfree 破坏链表）
    slab_bit_clear(sp, obj_idx);
    *(void **)ptr = sp->free_list;
    sp->free_list = ptr;
    sp->in_use--;

    // slab_validate_sp(sp, "kfree-post", obj_size);

    // 如果页面刚从满状态转变出来（只要减去 1 后等于 total - 1，那它之前一定在 full 链表中）
    if (sp->in_use == sp->total - 1) {
        slab_list_remove(&c->full, sp);
        sp->state = SLAB_STATE_PARTIAL;
        slab_list_push(&c->partial, sp);
    }

    // 如果页面已经完全空闲（注意这里用 if 而不是 else if，以处理 total == 1 的极端情况）
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
    spin_unlock_irqrestore(&c->lock, irq_flags);
}

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    // 确定原始大小
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

    // 如果新大小不大于旧大小，直接返回原指针
    if (new_size <= old_size) return ptr;

    // 分配新内存并拷贝数据
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size);
    kfree(ptr);
    return new_ptr;
}

// 分配并清零内存
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

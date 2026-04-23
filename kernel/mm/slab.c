#include "slab.h"
#include "frame.h"
#include "string.h"
#include "panic.h"
#include "stdio.h"

#define SLAB_NR_CACHES  7  // Slab 缓存数量
#define SLAB_MAX_OBJ   2048  // 最大对象大小，超过此大小直接使用 buddy 分配器
#define SLAB_HDR_SIZE   64  // Slab 页面头部大小

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
    uint8_t  cache_idx;      // 所属缓存索引
} slab_page_t;

// Slab 缓存结构
typedef struct {
    size_t         obj_size;      // 对象大小
    size_t         objs_per_slab; // 每页可容纳的对象数
    slab_page_t   *partial;       // 部分使用的页面链表
    slab_page_t   *full;          // 已满的页面链表
    slab_page_t   *spare;         // 备用空闲页面
} slab_cache_t;

// 全局 Slab 缓存数组
static slab_cache_t caches[SLAB_NR_CACHES];

// Slab 分配器初始化
void slab_init(void) {
    for (int i = 0; i < SLAB_NR_CACHES; i++) {
        caches[i].obj_size     = slab_sizes[i];
        caches[i].objs_per_slab = (PAGE_SIZE - SLAB_HDR_SIZE) / slab_sizes[i];
        caches[i].partial = NULL;
        caches[i].full    = NULL;
        caches[i].spare   = NULL;
    }
}

// 为指定缓存分配一个新的 Slab 页面并初始化
static slab_page_t *slab_grow(int idx) {
    slab_cache_t *c = &caches[idx];
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) return NULL;

    slab_page_t *sp = (slab_page_t *)pfn_to_virt(pfn);
    sp->cache_idx = (uint8_t)idx;
    sp->in_use    = 0;
    sp->total     = (uint16_t)c->objs_per_slab;
    sp->prev      = NULL;
    sp->next      = NULL;

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

// 验证 Slab 页面的完整性（调试用）
static void slab_validate_sp(slab_page_t *sp, const char *where, size_t obj_size) {
    int free_count = 0;
    for (void *p = sp->free_list; p; p = *(void **)p) {
        uintptr_t offset = (uintptr_t)p - (uintptr_t)sp;
        if ((offset - SLAB_HDR_SIZE) % obj_size != 0 || offset >= PAGE_SIZE) {
            printf("[SLAB BUG] %s: corrupted free_list node=%p sp=%p offset=%lu obj_size=%zu\n",
                   where, p, (void *)sp, (unsigned long)offset, obj_size);
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

    // 超过最大对象大小，直接使用 buddy 分配器
    if (size >= SLAB_MAX_OBJ) {
        int order = 0;
        size_t need = ROUND_UP(size + sizeof(size_t), PAGE_SIZE);
        while ((1u << order) * PAGE_SIZE < need) order++;
        if (order > MAX_ORDER) return NULL;
        pfn_t pfn = pfa_alloc(order);
        if (pfn == PFN_NONE) return NULL;
        size_t *hdr = (size_t *)pfn_to_virt(pfn);
        *hdr = (size_t)order;  // 在头部存储 order，方便释放时使用
        return (void *)(hdr + 1);
    }

    // 选择合适的缓存大小
    int idx = 0;
    while (idx < SLAB_NR_CACHES - 1 && slab_sizes[idx] < size) idx++;

    slab_cache_t *c = &caches[idx];
    slab_page_t *sp = c->partial;

    // 如果没有部分使用的页面，使用备用页面或分配新页面
    if (!sp) {
        sp = c->spare;
        if (sp) {
            c->spare = NULL;
        } else {
            sp = slab_grow(idx);
            if (!sp) return NULL;
        }
        slab_list_push(&c->partial, sp);
    }

    size_t obj_size = c->obj_size;
    slab_validate_sp(sp, "kmalloc-pre", obj_size);

    // 从空闲链表取出一个对象
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
    sp->free_list = *(void **)obj;
    sp->in_use++;

    // 如果页面已满，移动到 full 链表
    if (sp->in_use == sp->total) {
        slab_list_remove(&c->partial, sp);
        slab_list_push(&c->full, sp);
    }

    return obj;
}

// 释放 Slab 分配的内存
void kfree(void *ptr) {
    if (!ptr) return;
    uint64_t caller_ra = arch_read_ra();

    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)sp;

    // 如果不是 Slab 管理的对象（大对象），使用 buddy 释放
    if (offset < SLAB_HDR_SIZE || sp->cache_idx >= SLAB_NR_CACHES) {
        size_t *hdr = (size_t *)ptr - 1;
        int order = (int)*hdr;
        pfn_t pfn = virt_to_pfn(hdr);
        if (pfn_valid(pfn)) pfa_free(pfn, order);
        return;
    }

    /* sanity check: ptr must be aligned to obj_size and within the page */
    size_t obj_size = slab_sizes[sp->cache_idx];
    if ((offset - SLAB_HDR_SIZE) % obj_size != 0 || offset >= PAGE_SIZE) {
        printf("[SLAB BUG] kfree(%p) bad offset=%lu sp=%p cache_idx=%u obj_size=%zu\n",
               ptr, (unsigned long)offset, (void *)sp, sp->cache_idx, obj_size);
        panic("kfree: corrupted slab pointer");
    }

    slab_validate_sp(sp, "kfree-pre", obj_size);

    // 检查是否双重释放
    for (void *p = sp->free_list; p; p = *(void **)p) {
        if (p == ptr) {
            printf("[SLAB BUG] kfree(%p): double free detected sp=%p cache_idx=%u ra=0x%lx\n",
                   ptr, (void *)sp, sp->cache_idx, (unsigned long)caller_ra);
            panic("kfree: double free");
        }
    }

    // 将对象放回空闲链表
    *(void **)ptr = sp->free_list;
    sp->free_list = ptr;
    sp->in_use--;

    slab_validate_sp(sp, "kfree-post", obj_size);

    int idx = sp->cache_idx;
    slab_cache_t *c = &caches[idx];

    // 如果页面从满变为部分使用，移动到 partial 链表
    if (sp->in_use == sp->total - 1 && sp == c->full) {
        slab_list_remove(&c->full, sp);
        slab_list_push(&c->partial, sp);
    } else if (sp->in_use == 0 && sp != c->spare) {
        // 如果页面完全空闲，可以作为备用页面或释放
        if (sp == c->full) slab_list_remove(&c->full, sp);
        else if (sp == c->partial) slab_list_remove(&c->partial, sp);
        if (!c->spare) {
            c->spare = sp;
        } else {
            pfn_t pfn = virt_to_pfn(sp);
            pfa_free_page(pfn);
        }
    }
}

// 重新分配内存（调整大小）
void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    // 确定原始大小
    size_t old_size;
    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)sp;
    if (offset < SLAB_HDR_SIZE || sp->cache_idx >= SLAB_NR_CACHES) {
        size_t *hdr = (size_t *)ptr - 1;
        old_size = ((size_t)1 << (int)*hdr) * PAGE_SIZE - sizeof(size_t);
    } else {
        old_size = slab_sizes[sp->cache_idx];
    }

    // 如果新大小不大于旧大小，直接返回原指针
    if (new_size <= old_size) return ptr;

    // 分配新内存并拷贝数据
    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
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

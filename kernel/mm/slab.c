#include "slab.h"
#include "frame.h"
#include "string.h"
#include "panic.h"
#include "stdio.h"

#define SLAB_NR_CACHES  7
#define SLAB_MAX_OBJ   2048
#define SLAB_HDR_SIZE   64

static const size_t slab_sizes[SLAB_NR_CACHES] = {
    32, 64, 128, 256, 512, 1024, 2048
};

typedef struct slab_page {
    struct slab_page *next;
    struct slab_page *prev;
    uint16_t in_use;
    uint16_t total;
    void    *free_list;
    uint8_t  cache_idx;
} slab_page_t;

typedef struct {
    size_t         obj_size;
    size_t         objs_per_slab;
    slab_page_t   *partial;
    slab_page_t   *full;
    slab_page_t   *spare;
} slab_cache_t;

static slab_cache_t caches[SLAB_NR_CACHES];

void slab_init(void) {
    for (int i = 0; i < SLAB_NR_CACHES; i++) {
        caches[i].obj_size     = slab_sizes[i];
        caches[i].objs_per_slab = (PAGE_SIZE - SLAB_HDR_SIZE) / slab_sizes[i];
        caches[i].partial = NULL;
        caches[i].full    = NULL;
        caches[i].spare   = NULL;
    }
}

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

void *kmalloc(size_t size) {
    if (size == 0) return NULL;

    if (size >= SLAB_MAX_OBJ) {
        int order = 0;
        size_t need = ROUND_UP(size + sizeof(size_t), PAGE_SIZE);
        while ((1u << order) * PAGE_SIZE < need) order++;
        if (order > MAX_ORDER) return NULL;
        pfn_t pfn = pfa_alloc(order);
        if (pfn == PFN_NONE) return NULL;
        size_t *hdr = (size_t *)pfn_to_virt(pfn);
        *hdr = (size_t)order;
        return (void *)(hdr + 1);
    }

    int idx = 0;
    while (idx < SLAB_NR_CACHES - 1 && slab_sizes[idx] < size) idx++;

    slab_cache_t *c = &caches[idx];
    slab_page_t *sp = c->partial;

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

    if (sp->in_use == sp->total) {
        slab_list_remove(&c->partial, sp);
        slab_list_push(&c->full, sp);
    }

    return obj;
}

void kfree(void *ptr) {
    if (!ptr) return;
    uint64_t caller_ra;
    __asm__ volatile("mv %0, ra" : "=r"(caller_ra));

    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)sp;

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

    for (void *p = sp->free_list; p; p = *(void **)p) {
        if (p == ptr) {
            printf("[SLAB BUG] kfree(%p): double free detected sp=%p cache_idx=%u ra=0x%lx\n",
                   ptr, (void *)sp, sp->cache_idx, (unsigned long)caller_ra);
            panic("kfree: double free");
        }
    }

    *(void **)ptr = sp->free_list;
    sp->free_list = ptr;
    sp->in_use--;

    slab_validate_sp(sp, "kfree-post", obj_size);

    int idx = sp->cache_idx;
    slab_cache_t *c = &caches[idx];

    if (sp->in_use == sp->total - 1 && sp == c->full) {
        slab_list_remove(&c->full, sp);
        slab_list_push(&c->partial, sp);
    } else if (sp->in_use == 0 && sp != c->spare) {
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

void *krealloc(void *ptr, size_t new_size) {
    if (!ptr) return kmalloc(new_size);
    if (new_size == 0) { kfree(ptr); return NULL; }

    size_t old_size;
    slab_page_t *sp = (slab_page_t *)((uintptr_t)ptr & ~(PAGE_SIZE - 1));
    uintptr_t offset = (uintptr_t)ptr - (uintptr_t)sp;
    if (offset < SLAB_HDR_SIZE || sp->cache_idx >= SLAB_NR_CACHES) {
        size_t *hdr = (size_t *)ptr - 1;
        old_size = ((size_t)1 << (int)*hdr) * PAGE_SIZE - sizeof(size_t);
    } else {
        old_size = slab_sizes[sp->cache_idx];
    }

    if (new_size <= old_size) return ptr;

    void *new_ptr = kmalloc(new_size);
    if (!new_ptr) return NULL;
    memcpy(new_ptr, ptr, old_size < new_size ? old_size : new_size);
    kfree(ptr);
    return new_ptr;
}

void *kcalloc(size_t nmemb, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(nmemb, size, &total)) return NULL;
    void *p = kmalloc(total);
    if (p) memset(p, 0, total);
    return p;
}

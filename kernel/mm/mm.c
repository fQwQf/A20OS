#include "defs.h"
#include "mm.h"
#include "frame.h"
#include "slab.h"
#include "panic.h"
#include "stdio.h"
#include "string.h"

static inline paddr_t va_to_pa(const void *va) {
    return (paddr_t)((uint64_t)(uintptr_t)va - PAGE_OFFSET);
}

void mm_init(void) {
    extern char _bss_end[];
    pfa_init(PHYS_MEMORY_BASE, PHYS_MEMORY_END,
             va_to_pa(_bss_end));
    slab_init();
    printf("[MM] Buddy+Slab: %d frames, %d free (%d MB)\n",
           (int)pfa.total_frames, (int)pfa.free_frames,
           (int)(pfa.free_frames * PAGE_SIZE / 1024 / 1024));
}

void *frame_alloc(void) {
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) return NULL;
    void *p = pfn_to_virt(pfn);
    memset(p, 0, PAGE_SIZE);
    return p;
}

void frame_free(void *addr) {
    if (!addr) return;
    pfn_t pfn = virt_to_pfn(addr);
    if (pfn_valid(pfn))
        pfa_free_page(pfn);
}

size_t frame_free_count(void) {
    return pfa_free_count();
}

uint64_t *pt_create(void) {
    return (uint64_t *)frame_alloc();
}

void pt_destroy(uint64_t *pgdir) {
    if (!pgdir) return;
    for (int i = 0; i < 512; i++) {
        uint64_t pte = pgdir[i];
        if ((pte & PTE_V) && !(pte & PTE_R) && !(pte & PTE_W) && !(pte & PTE_X)) {
            uint64_t *next = PTE_TO_PTR(pte);
            pt_destroy(next);
            pgdir[i] = 0;
        }
    }
    frame_free(pgdir);
}

uint64_t *pt_walk(uint64_t *pgdir, vaddr_t va, int alloc) {
    uint64_t *table = pgdir;
    for (int level = 2; level > 0; level--) {
        int vpn = SV39_VPN(va, level);
        uint64_t pte = table[vpn];
        if (pte & PTE_V) {
            table = PTE_TO_PTR(pte);
        } else {
            if (!alloc) return NULL;
            uint64_t *next = (uint64_t *)frame_alloc();
            if (!next) return NULL;
            table[vpn] = PTE_FROM_PA(va_to_pa(next)) | PTE_V;
            table = next;
        }
    }
    return &table[SV39_VPN(va, 0)];
}

int pt_map(uint64_t *pgdir, vaddr_t va, paddr_t pa, uint64_t flags) {
    uint64_t *pte = pt_walk(pgdir, va, 1);
    if (!pte) return -ENOMEM;
    if (*pte & PTE_V) {
        paddr_t old_pa = SV39_PTE_ADDR(*pte);
        if (old_pa != pa) {
            int is_leaf = (*pte & PTE_R) || (*pte & PTE_W) || (*pte & PTE_X);
            if (is_leaf)
                frame_put(phys_to_pfn(old_pa));
        }
    }
    *pte = PTE_FROM_PA(pa) | flags | PTE_V;
    return 0;
}

int pt_unmap(uint64_t *pgdir, vaddr_t va) {
    uint64_t *pte = pt_walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_V)) return -EINVAL;
    *pte = 0;
    return 0;
}

paddr_t pt_translate(uint64_t *pgdir, vaddr_t va) {
    uint64_t *pte = pt_walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_V)) return 0;
    return SV39_PTE_ADDR(*pte) | (va & 0xFFF);
}

void pt_map_kernel(uint64_t *pgdir) {
    for (int i = 256; i < 512; i++) {
        if (boot_pgdir[i] & PTE_V)
            pgdir[i] = boot_pgdir[i];
    }
}

int pt_map_range(uint64_t *pgdir, vaddr_t va, paddr_t pa, size_t size, uint64_t flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        int r = pt_map(pgdir, va + off, pa + off, flags);
        if (r < 0) return r;
    }
    return 0;
}

static uint64_t *pt_clone_level(uint64_t *src, int level) {
    uint64_t *dst = (uint64_t *)frame_alloc();
    if (!dst) return NULL;

    for (int i = 0; i < 512; i++) {
        uint64_t pte = src[i];
        if (!(pte & PTE_V)) continue;

        int is_leaf = (pte & PTE_R) || (pte & PTE_W) || (pte & PTE_X);

        if (is_leaf) {
            if (pte & PTE_U) {
                void *nf = frame_alloc();
                if (!nf) { pt_destroy(dst); return NULL; }
                memcpy(nf, PTE_TO_PTR(pte), PAGE_SIZE);
                dst[i] = PTE_FROM_PA(va_to_pa(nf)) | (pte & 0x3FF);
            } else {
                dst[i] = pte;
            }
        } else {
            uint64_t *next_src = PTE_TO_PTR(pte);
            uint64_t *next_dst = pt_clone_level(next_src, level - 1);
            if (!next_dst) { pt_destroy(dst); return NULL; }
            dst[i] = PTE_FROM_PA(va_to_pa(next_dst)) | PTE_V;
        }
    }
    return dst;
}

uint64_t *pt_clone(uint64_t *src_pgdir) {
    if (!src_pgdir) return NULL;
    return pt_clone_level(src_pgdir, 2);
}

static void pt_destroy_user_recursive(uint64_t *table, int level) {
    if (!table) return;
    /* Only the user half (0..255) lives at the root; kernel half is shared
     * and must not be freed.  Lower levels may span all 512 entries. */
    int limit = (level == 2) ? 256 : 512;
    for (int i = 0; i < limit; i++) {
        uint64_t pte = table[i];
        if (!(pte & PTE_V)) continue;

        int is_leaf = (pte & PTE_R) || (pte & PTE_W) || (pte & PTE_X);

        if (is_leaf) {
            if (pte & PTE_U)
                frame_put(phys_to_pfn(SV39_PTE_ADDR(pte)));
            table[i] = 0;
        } else {
            uint64_t *next = PTE_TO_PTR(pte);
            pt_destroy_user_recursive(next, level - 1);
            frame_free(next);
            table[i] = 0;
        }
    }
}

void pt_destroy_user(uint64_t *pgdir) {
    if (!pgdir) return;
    pt_destroy_user_recursive(pgdir, 2);
    frame_free(pgdir);
}

#include "vm.h"
#include "proc.h"
#include "trap.h"

long copy_from_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    size_t copied = 0;
    while (n > 0) {
        uint64_t va = (uint64_t)src + copied;
        if (va >= 0x4000000000UL) return -EFAULT;
        uint64_t *pte = pt_walk(t->pgdir, va, 0);
        if (!pte || !(*pte & PTE_V)) {
            if (handle_demand_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V)) return -EFAULT;
        }
        paddr_t pa = SV39_PTE_ADDR(*pte);
        size_t page_off = va & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > n) chunk = n;
        memcpy((char*)dst + copied, (void*)(pa + PAGE_OFFSET + page_off), chunk);
        copied += chunk;
        n -= chunk;
    }
    return (long)copied;
}

long copy_to_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    size_t copied = 0;
    while (n > 0) {
        uint64_t va = (uint64_t)dst + copied;
        if (va >= 0x4000000000UL) return -EFAULT;
        uint64_t *pte = pt_walk(t->pgdir, va, 0);
        if (!pte || !(*pte & PTE_V)) {
            if (handle_demand_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V)) return -EFAULT;
        }
        if (!(*pte & PTE_W)) {
            if (handle_cow_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V) || !(*pte & PTE_W)) return -EFAULT;
        }
        paddr_t pa = SV39_PTE_ADDR(*pte);
        size_t page_off = va & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > n) chunk = n;
        memcpy((void*)(pa + PAGE_OFFSET + page_off), (const char*)src + copied, chunk);
        copied += chunk;
        n -= chunk;
    }
    return (long)copied;
}

long user_strncpy(char *dst, const char *src, size_t max) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    if (max == 0) return -EINVAL;
    size_t i = 0;
    while (i < max - 1) {
        uint64_t va = (uint64_t)(src + i);
        if (va >= 0x4000000000UL) return -EFAULT;
        uint64_t *pte = pt_walk(t->pgdir, va, 0);
        if (!pte || !(*pte & PTE_V)) {
            if (handle_demand_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V)) return -EFAULT;
        }
        paddr_t pa = SV39_PTE_ADDR(*pte);
        size_t page_off = va & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > max - 1 - i) chunk = max - 1 - i;
        const char *src_page = (const char *)(pa + PAGE_OFFSET + page_off);
        for (size_t j = 0; j < chunk; j++) {
            dst[i + j] = src_page[j];
            if (src_page[j] == '\0') return (long)(i + j);
        }
        i += chunk;
    }
    dst[i] = '\0';
    return (long)i;
}

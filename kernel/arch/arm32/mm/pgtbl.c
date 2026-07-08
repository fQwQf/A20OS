#ifndef CONFIG_NOMMU
#include "core/defs.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vm.h"
#include "mm/fault.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"
#include "core/klog.h"
#include "proc/proc.h"

#define ARM32_L1_SIZE      (ARCH_PT_ENTRIES * sizeof(pte_t))
#define ARM32_L2_ENTRIES   256
#define ARM32_L2_SIZE      (ARM32_L2_ENTRIES * sizeof(pte_t))

#define L1_INDEX(va)       ((va) >> 20)
#define L2_INDEX(va)       (((va) >> 12) & 0xFFU)

#define IS_TABLE(pte)      (((pte) & 0x3U) == ARM32_L1_TYPE_TABLE)
#define IS_LEAF(pte)       (((pte) & 0x3U) == ARM32_L2_TYPE_SMALL)

static inline size_t pt_level_size(int level) {
    return PAGE_SIZE << (ARCH_PT_BITS * level);
}

static inline int l1_index_valid(int idx) {
    return idx >= 0 && idx < ARCH_PT_ENTRIES;
}

static pte_t *pt_create_internal(int order) {
    pfn_t pfn = pfa_alloc(order);
    if (pfn == PFN_NONE)
        return NULL;
    void *va = pfn_to_virt(pfn);
    memset(va, 0, PAGE_SIZE << order);
    return (pte_t *)va;
}

static void pt_free_internal(pte_t *table, int order) {
    if (!table)
        return;
    pfn_t pfn = virt_to_pfn(table);
    if (pfn_valid(pfn))
        pfa_free(pfn, order);
}

static uint32_t arm32_section_pte(paddr_t pa, pte_t flags) {
    uint32_t pte = (pa & 0xFFF00000U) | ARM32_L1_TYPE_SECTION | PTE_V;

    if (flags & PTE_W) {
        pte |= PTE_W;
        pte |= (1U << 10);
    }
    if (flags & PTE_U) {
        pte |= PTE_U;
        pte |= (1U << 11);
    }
    if (flags & PTE_COW)
        pte |= PTE_COW;
    if (flags & PTE_MAT1)
        pte |= ARM32_PTE_C;

    pte |= ARM32_PTE_B;
    pte |= (flags & PTE_LEAF);
    return pte;
}

pte_t *pt_create(void) {
    return pt_create_internal(2);
}

static void pt_destroy_recursive(pte_t *table, int level) {
    int entries;

    if (!table)
        return;
    entries = (level == ARCH_PT_ROOT_LEVEL) ? ARCH_PT_ENTRIES : ARM32_L2_ENTRIES;

    for (int i = 0; i < entries; i++) {
        pte_t pte = table[i];
        if (!(pte & PTE_V))
            continue;
        if (IS_TABLE(pte)) {
            pte_t *next = arch_pte_to_ptr(pte);
            pt_destroy_recursive(next, level - 1);
            pt_free_internal(next, 0);
            table[i] = 0;
        }
    }
}

void pt_destroy(pt_root_t *pgdir) {
    if (!pgdir)
        return;
    pt_destroy_recursive(pgdir, ARCH_PT_ROOT_LEVEL);
    pt_free_internal(pgdir, 2);
}

pte_t *pt_walk(pt_root_t *pgdir, vaddr_t va, int alloc) {
    int idx;
    pte_t pte;
    pte_t *table;
    pte_t *next;

    if (!pgdir)
        return NULL;

    idx = L1_INDEX(va);
    if (!l1_index_valid(idx))
        return NULL;

    table = pgdir;
    pte = table[idx];
    if (pte & PTE_V) {
        if (IS_LEAF(pte))
            return NULL;
        if (!IS_TABLE(pte))
            return NULL;
        table = arch_pte_to_ptr(pte);
    } else {
        if (!alloc)
            return NULL;
        next = (pte_t *)frame_alloc();
        if (!next)
            return NULL;
        table[idx] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
        table = next;
    }

    return &table[L2_INDEX(va)];
}

pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t va, int *level_out,
                      vaddr_t *base_out, size_t *size_out) {
    int idx;
    pte_t pte;
    pte_t *table;
    pte_t *pte_p;

    if (!pgdir)
        return NULL;

    idx = L1_INDEX(va);
    if (!l1_index_valid(idx))
        return NULL;

    table = pgdir;
    pte = table[idx];
    if (!(pte & PTE_V))
        return NULL;

    if (IS_LEAF(pte)) {
        if (level_out)
            *level_out = ARCH_PT_ROOT_LEVEL;
        if (base_out)
            *base_out = va & ~((vaddr_t)(PMD_SIZE - 1));
        if (size_out)
            *size_out = PMD_SIZE;
        return &table[idx];
    }

    if (!IS_TABLE(pte))
        return NULL;

    table = arch_pte_to_ptr(pte);
    pte_p = &table[L2_INDEX(va)];
    if (!(*pte_p & PTE_V) || !IS_LEAF(*pte_p))
        return NULL;

    if (level_out)
        *level_out = 0;
    if (base_out)
        *base_out = va & ~((vaddr_t)(PAGE_SIZE - 1));
    if (size_out)
        *size_out = PAGE_SIZE;
    return pte_p;
}

int mm_query_leaf(pt_root_t *pgdir, vaddr_t va, mm_leaf_info_t *out) {
    int level;
    vaddr_t base;
    size_t size;
    pte_t *pte;

    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!pgdir)
        return 0;

    level = 0;
    base = 0;
    size = 0;
    pte = pt_lookup_leaf(pgdir, va, &level, &base, &size);
    if (!pte || !(*pte & PTE_V) || !IS_LEAF(*pte))
        return 0;

    out->level = level;
    out->base = base;
    out->size = size;
    out->pa = arch_pte_addr(*pte) + (va - base);
    out->flags = arch_pte_flags(*pte);
    out->dirty = 0;
    return 1;
}

uint64_t mm_pagemap_entry(pt_root_t *pgdir, vaddr_t va) {
    mm_leaf_info_t info;
    uint64_t pfn;

    if (!mm_query_leaf(pgdir, va, &info))
        return 0;
    pfn = info.pa / PAGE_SIZE;
    return (1ULL << 63) | (pfn & 0x7FFFFFFFFFFFFULL);
}

int mm_query_leaf_kaddr(pt_root_t *pgdir, vaddr_t va, void **kaddr_out,
                        size_t *avail_out) {
    mm_leaf_info_t info;

    if (!kaddr_out || !avail_out)
        return 0;
    *kaddr_out = NULL;
    *avail_out = 0;

    if (!mm_query_leaf(pgdir, va, &info))
        return 0;
    *kaddr_out = (void *)(info.pa + PAGE_OFFSET);
    *avail_out = info.size - (va - info.base);
    return 1;
}

int mm_fetch_user_insn32(pt_root_t *pgdir, vaddr_t va, uint32_t *out) {
    void *kaddr;
    size_t avail;

    if (!out)
        return 0;
    if (!mm_query_leaf_kaddr(pgdir, va, &kaddr, &avail) || avail < sizeof(uint32_t))
        return 0;
    *out = *(uint32_t *)kaddr;
    return 1;
}

int mm_mark_leaf_dirty_if_writable(pt_root_t *pgdir, vaddr_t va) {
    pte_t *pte;
    pte_t flags;

    if (!pgdir)
        return -1;
    pte = pt_lookup_leaf(pgdir, va, NULL, NULL, NULL);
    if (!pte || !(*pte & PTE_V) || !(*pte & PTE_W))
        return -1;
    flags = arch_pte_flags(*pte);
    *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
    arch_tlb_flush_page(va);
    return 0;
}

int mm_debug_pte_value(pt_root_t *pgdir, vaddr_t va, uintptr_t *slot_out,
                       pte_t *value_out) {
    pte_t *pte;

    if (slot_out)
        *slot_out = 0;
    if (value_out)
        *value_out = 0;
    if (!pgdir)
        return 0;
    pte = pt_lookup_leaf(pgdir, va, NULL, NULL, NULL);
    if (slot_out)
        *slot_out = (uintptr_t)pte;
    if (value_out)
        *value_out = pte ? *pte : 0;
    return pte != NULL;
}

int pt_map(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags) {
    pte_t *pte;
    paddr_t old_pa;

    pte = pt_walk(pgdir, va, 1);
    if (!pte)
        return -ENOMEM;

    if (*pte & PTE_V) {
        old_pa = arch_pte_addr(*pte);
        if (old_pa != pa && IS_LEAF(*pte))
            frame_put(phys_to_pfn(old_pa));
    }

    *pte = arch_pte_leaf(pa, flags);
    return 0;
}

int pt_map_huge(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags) {
    int idx;
    pte_t *pte;

    if (!pgdir)
        return -EINVAL;
    if ((va & (PMD_SIZE - 1)) || (pa & (PMD_SIZE - 1)))
        return -EINVAL;

    idx = L1_INDEX(va);
    if (!l1_index_valid(idx))
        return -EINVAL;

    pte = &pgdir[idx];
    if (*pte & PTE_V)
        return -EEXIST;

    *pte = arm32_section_pte(pa, flags);
    return 0;
}

static int pt_table_empty(pte_t *table, int entries) {
    for (int i = 0; i < entries; i++) {
        if (table[i] & PTE_V)
            return 0;
    }
    return 1;
}

int pt_unmap(pt_root_t *pgdir, vaddr_t va) {
    int idx1;
    pte_t pte;
    pte_t *l2;
    int idx2;
    pte_t *leaf;

    if (!pgdir)
        return -EINVAL;

    idx1 = L1_INDEX(va);
    if (!l1_index_valid(idx1))
        return -EINVAL;

    pte = pgdir[idx1];
    if (!(pte & PTE_V) || IS_LEAF(pte))
        return -EINVAL;
    if (!IS_TABLE(pte))
        return -EINVAL;

    l2 = arch_pte_to_ptr(pte);
    idx2 = L2_INDEX(va);
    leaf = &l2[idx2];
    if (!(*leaf & PTE_V) || !IS_LEAF(*leaf))
        return -EINVAL;

    *leaf = 0;
    if (pt_table_empty(l2, ARM32_L2_ENTRIES)) {
        pt_free_internal(l2, 0);
        pgdir[idx1] = 0;
    }
    return 0;
}

int pt_unmap_leaf(pt_root_t *pgdir, vaddr_t va, paddr_t *pa_out,
                  vaddr_t *base_out, size_t *size_out, int *level_out) {
    int idx1;
    pte_t pte;
    pte_t *l2;
    int idx2;
    pte_t *leaf;
    vaddr_t base;

    if (!pgdir)
        return -EINVAL;

    idx1 = L1_INDEX(va);
    if (!l1_index_valid(idx1))
        return -EINVAL;

    pte = pgdir[idx1];
    if (!(pte & PTE_V))
        return -EINVAL;

    if (IS_LEAF(pte)) {
        base = va & ~((vaddr_t)(PMD_SIZE - 1));
        pgdir[idx1] = 0;
        if (pa_out)
            *pa_out = arch_pte_addr(pte);
        if (base_out)
            *base_out = base;
        if (size_out)
            *size_out = PMD_SIZE;
        if (level_out)
            *level_out = ARCH_PT_ROOT_LEVEL;
        return 0;
    }

    if (!IS_TABLE(pte))
        return -EINVAL;

    l2 = arch_pte_to_ptr(pte);
    idx2 = L2_INDEX(va);
    leaf = &l2[idx2];
    if (!(*leaf & PTE_V) || !IS_LEAF(*leaf))
        return -EINVAL;

    base = va & ~((vaddr_t)(PAGE_SIZE - 1));
    if (pa_out)
        *pa_out = arch_pte_addr(*leaf);
    if (base_out)
        *base_out = base;
    if (size_out)
        *size_out = PAGE_SIZE;
    if (level_out)
        *level_out = 0;

    *leaf = 0;
    if (pt_table_empty(l2, ARM32_L2_ENTRIES)) {
        pt_free_internal(l2, 0);
        pgdir[idx1] = 0;
    }
    return 0;
}

paddr_t pt_translate(pt_root_t *pgdir, vaddr_t va) {
    vaddr_t base;
    size_t size;
    pte_t *pte;

    base = 0;
    size = 0;
    pte = pt_lookup_leaf(pgdir, va, NULL, &base, &size);
    if (!pte || !(*pte & PTE_V) || !IS_LEAF(*pte))
        return 0;
    return arch_pte_addr(*pte) + (va - base);
}

static inline pte_t *boot_pgdir_virt(void) {
    extern uint32_t boot_pgdir[];
    return (pte_t *)((uintptr_t)boot_pgdir + PAGE_OFFSET);
}

void pt_map_kernel(pt_root_t *pgdir) {
    if (!pgdir)
        return;
    int start = PAGE_OFFSET >> 20;
    pte_t *src = boot_pgdir_virt();
    memcpy(&pgdir[start], &src[start],
           (ARCH_PT_ENTRIES - start) * sizeof(pte_t));
}

int pt_map_range(pt_root_t *pgdir, vaddr_t va, paddr_t pa, size_t size, pte_t flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        int r = pt_map(pgdir, va + off, pa + off, flags);
        if (r < 0)
            return r;
    }
    return 0;
}

static pte_t *pt_clone_level(pte_t *src, int level);

static pte_t *pt_clone_l2(pte_t *src) {
    pte_t *dst = (pte_t *)frame_alloc();
    pte_t pte;
    paddr_t pa;
    pfn_t pfn;
    pfn_t np;

    if (!dst)
        return NULL;

    for (int i = 0; i < ARM32_L2_ENTRIES; i++) {
        pte = src[i];
        if (!(pte & PTE_V))
            continue;
        if (!IS_LEAF(pte)) {
            frame_free(dst);
            return NULL;
        }
        if (pte & PTE_U) {
            pa = arch_pte_addr(pte);
            pfn = phys_to_pfn(pa);
            if (!pfn_valid(pfn)) {
                frame_free(dst);
                return NULL;
            }
            np = pfa_alloc_page();
            if (np == PFN_NONE) {
                frame_free(dst);
                return NULL;
            }
            memcpy(pfn_to_virt(np), pfn_to_virt(pfn), PAGE_SIZE);
            frame_get(pfn);
            dst[i] = arch_pte_leaf(pfn_to_phys(np), arch_pte_flags(pte));
        } else {
            dst[i] = pte;
        }
    }
    return dst;
}

static pte_t *pt_clone_level(pte_t *src, int level) {
    pte_t *dst;
    int entries;
    pte_t pte;

    dst = (level == ARCH_PT_ROOT_LEVEL) ? pt_create() : (pte_t *)frame_alloc();
    if (!dst)
        return NULL;

    entries = (level == ARCH_PT_ROOT_LEVEL) ? ARCH_PT_ENTRIES : ARM32_L2_ENTRIES;
    for (int i = 0; i < entries; i++) {
        pte = src[i];
        if (!(pte & PTE_V))
            continue;

        if (IS_LEAF(pte)) {
            if (pte & PTE_U) {
                size_t leaf_size = pt_level_size(level);
                int order = (leaf_size == PMD_SIZE) ? PMD_ORDER : 0;
                pfn_t pfn = phys_to_pfn(arch_pte_addr(pte));
                pfn_t np;
                if (!pfn_valid(pfn)) {
                    pt_destroy(dst);
                    return NULL;
                }
                np = pfa_alloc(order);
                if (np == PFN_NONE) {
                    pt_destroy(dst);
                    return NULL;
                }
                memcpy(pfn_to_virt(np), pfn_to_virt(pfn), leaf_size);
                frame_get(pfn);
                if (level == ARCH_PT_ROOT_LEVEL)
                    dst[i] = arm32_section_pte(pfn_to_phys(np), arch_pte_flags(pte));
                else
                    dst[i] = arch_pte_leaf(pfn_to_phys(np), arch_pte_flags(pte));
            } else {
                dst[i] = pte;
            }
        } else if (IS_TABLE(pte)) {
            pte_t *next_src = arch_pte_to_ptr(pte);
            pte_t *next_dst = pt_clone_l2(next_src);
            if (!next_dst) {
                pt_destroy(dst);
                return NULL;
            }
            dst[i] = arch_pte_from_pa(va_to_pa(next_dst)) | PTE_DIR;
        }
    }
    return dst;
}

pte_t *pt_clone(pt_root_t *src_pgdir) {
    if (!src_pgdir)
        return NULL;
    return pt_clone_level(src_pgdir, ARCH_PT_ROOT_LEVEL);
}

static void pt_destroy_user_recursive(pte_t *table, int level) {
    int limit;
    pte_t pte;
    paddr_t next_pa;
    pfn_t next_pfn;
    pte_t *next;

    if (!table)
        return;

    limit = (level == ARCH_PT_ROOT_LEVEL) ? ARCH_PT_USER_END : ARM32_L2_ENTRIES;
    for (int i = 0; i < limit; i++) {
        pte = table[i];
        if (!(pte & PTE_V))
            continue;

        if (IS_LEAF(pte)) {
            if (pte & PTE_U)
                frame_put(phys_to_pfn(arch_pte_addr(pte)));
            table[i] = 0;
        } else if (IS_TABLE(pte)) {
            next_pa = arch_pte_addr(pte);
            next_pfn = phys_to_pfn(next_pa);
            if ((next_pa & 0x3FFU) || !pfn_valid(next_pfn)) {
                kerr("pt_destroy_user: skip invalid non-leaf level=%d idx=%d pte=0x%lx pa=0x%lx\n",
                     level, i, (unsigned long)pte, (unsigned long)next_pa);
                table[i] = 0;
                continue;
            }
            next = arch_pte_to_ptr(pte);
            pt_destroy_user_recursive(next, level - 1);
            frame_free(next);
            table[i] = 0;
        }
    }
}

void pt_destroy_user(pt_root_t *pgdir) {
    if (!pgdir)
        return;
    pt_destroy_user_recursive(pgdir, ARCH_PT_ROOT_LEVEL);
    pt_free_internal(pgdir, 2);
}

#endif /* CONFIG_NOMMU */

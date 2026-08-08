#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "fs/page_cache.h"
#include "proc/proc.h"
#include "core/string.h"

/*
 * Fork / copy-on-write page-table cloning.
 *
 * mm_fork() (in mm/vm.c) drives the VMA snapshot and calls these helpers while
 * holding parent->lock so the parent cannot race a concurrent page fault.
 * The clone walks only already-present leaf PTEs; file-backed shared mappings
 * are left populated on demand.
 */

static size_t vm_pt_level_size(int level) {
    return PAGE_SIZE << (ARCH_PT_BITS * level);
}

static uint64_t mm_cow_flags(uint64_t pte) {
    uint64_t flags = pte & (PTE_R | PTE_W | PTE_X | PTE_U | PTE_A |
                            PTE_D | PTE_G | PTE_MAT1 | PTE_LEAF |
                            PTE_COW);
    if (pte & (PTE_W | PTE_COW)) {
        flags &= ~(uint64_t)(PTE_W | PTE_D);
        flags |= PTE_COW;
    }
    return flags;
}

int mm_fork_clone_page(mm_struct_t *child, mm_struct_t *parent, vaddr_t va,
                       int shared) {
    int level = 0;
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *src = pt_lookup_leaf(parent->pgdir, va, &level, &base, &size);
    if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src) || !(*src & PTE_U))
        return 0;
    if (va != base)
        return 0;

    pte_t *dst = pt_lookup_leaf(child->pgdir, va, NULL, NULL, NULL);
    if (dst && (*dst & PTE_V))
        return 0;

    paddr_t pa = arch_pte_addr(*src);
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -ENOMEM;

    if (!shared && arch_fork_requires_private_copy()) {
        pfn_t copy = pfa_alloc_page();
        if (copy == PFN_NONE)
            return -ENOMEM;
        memcpy(pfn_to_virt(copy), pfn_to_virt(pfn), PAGE_SIZE);
        int r = pt_map(child->pgdir, base, pfn_to_phys(copy),
                       arch_pte_flags(*src));
        if (r < 0) {
            frame_put(copy);
            return r;
        }
        child->rss++;
        return 0;
    }

    pte_t flags = shared ? arch_pte_flags(*src) : mm_cow_flags(*src);
    frame_get(pfn);

    int r = (level > 0) ? pt_map_huge(child->pgdir, base, pa, flags)
                        : pt_map(child->pgdir, base, pa, flags);
    if (r < 0) {
        frame_put(pfn);
        return r;
    }

    if (!shared && (*src & (PTE_W | PTE_COW))) {
        *src = arch_pte_leaf(pa, flags);
        mm_tlb_note_change(parent, base, size);
    }
    child->rss += size / PAGE_SIZE;
    return 0;
}

int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                        vaddr_t start, vaddr_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        int r = mm_fork_clone_page(child, parent, va, shared);
        if (r < 0)
            return r;
    }
    return 0;
}

int mm_fork_clone_leaf(mm_struct_t *child, mm_struct_t *parent,
                       pte_t *src_pte, vaddr_t va, int level,
                       int shared) {
    if (!src_pte || !(*src_pte & PTE_V) ||
        !arch_pte_is_leaf(*src_pte) || !(*src_pte & PTE_U))
        return 0;

    paddr_t pa = arch_pte_addr(*src_pte);
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -ENOMEM;

    size_t leaf_size = vm_pt_level_size(level);
    if (!shared && arch_fork_requires_private_copy()) {
        int order = (leaf_size == PMD_SIZE) ? PMD_ORDER : 0;
        if (leaf_size != PAGE_SIZE && leaf_size != PMD_SIZE)
            return -ENOMEM;
        pfn_t copy = pfa_alloc(order);
        if (copy == PFN_NONE)
            return -ENOMEM;
        memcpy(pfn_to_virt(copy), pfn_to_virt(pfn), leaf_size);
        int r = (level > 0) ?
            pt_map_huge(child->pgdir, va, pfn_to_phys(copy),
                        arch_pte_flags(*src_pte)) :
            pt_map(child->pgdir, va, pfn_to_phys(copy),
                   arch_pte_flags(*src_pte));
        if (r < 0) {
            pfa_free(copy, order);
            return r;
        }
        child->rss += leaf_size / PAGE_SIZE;
        return 0;
    }

    vm_area_t *vma = parent ? mm_find_vma(parent, va) : NULL;
    int shared_file = vma &&
        (vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED);
    page_cache_page_t *pcp = NULL;

    if (shared_file && vma->file_vnode) {
        uint64_t idx = vma->file_offset + (va - vma->start);
        idx /= PAGE_SIZE;
        pcp = page_cache_get(vma->file_vnode, idx, 0);
        if (!pcp)
            return -ENOMEM;
    } else if (!shared_file) {
        frame_get(pfn);
    }

    pte_t flags = shared ? arch_pte_flags(*src_pte) : mm_cow_flags(*src_pte);
    int r = (level > 0) ? pt_map_huge(child->pgdir, va, pa, flags)
                        : pt_map(child->pgdir, va, pa, flags);
    if (r < 0) {
        if (shared_file && pcp) {
            page_cache_put(pcp);
        } else if (!shared_file) {
            frame_put(pfn);
        }
        return r;
    }

    if (!shared && (*src_pte & (PTE_W | PTE_COW))) {
        *src_pte = arch_pte_leaf(pa, flags);
        mm_tlb_note_change(parent, va, leaf_size);
    }
    child->rss += vm_pt_level_size(level) / PAGE_SIZE;
    return 0;
}

int mm_fork_clone_present_level(mm_struct_t *child, mm_struct_t *parent,
                                pte_t *table, int level, vaddr_t base,
                                vaddr_t start, vaddr_t end, int shared) {
    if (!table || start >= end)
        return 0;

    size_t span = vm_pt_level_size(level);
    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        vaddr_t entry_base = base + (vaddr_t)i * span;
        vaddr_t entry_end = entry_base + span;
        if (entry_end <= start)
            continue;
        if (entry_base >= end)
            break;

        pte_t *pte = &table[i];
        if (!(*pte & PTE_V))
            continue;

        if (arch_pte_is_leaf(*pte)) {
            int r = mm_fork_clone_leaf(child, parent, pte, entry_base, level, shared);
            if (r < 0)
                return r;
            continue;
        }

        if (level > 0) {
            int r = mm_fork_clone_present_level(child, parent, arch_pte_to_ptr(*pte),
                                                level - 1, entry_base,
                                                start, end, shared);
            if (r < 0)
                return r;
        }
    }
    return 0;
}

int mm_fork_clone_present_range(mm_struct_t *child, mm_struct_t *parent,
                                vaddr_t start, vaddr_t end, int shared) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    return mm_fork_clone_present_level(child, parent, parent->pgdir, ARCH_PT_ROOT_LEVEL,
                                       0, start, end, shared);
}

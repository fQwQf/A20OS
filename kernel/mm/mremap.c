#include "mm/vm.h"
#include "mm/vm_internal.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "mm/vmo.h"
#include "mm/fault.h"
#include "mm/swap.h"
#include "fs/vfs.h"
#include "fs/page_cache.h"
#include "ipc/sysv_shm.h"
#include "proc/proc.h"
#include "proc/proc_internal.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"
#include "core/errno.h"

/* mremap: relocate/grow/shrink an existing mapping. */

static int mm_clone_shared_mapping(mm_struct_t *mm, vm_area_t *src_vma,
                                   vaddr_t src_addr, size_t len,
                                   int flags, vaddr_t new_addr,
                                   vaddr_t *out_addr) {
    if (!(src_vma->vm_flags & VM_SHARED))
        return -EINVAL;
    if (src_addr + len < src_addr || src_addr + len > src_vma->end)
        return -EINVAL;

    int prot = mm_pte_flags_to_prot(src_vma->pte_flags);
    vaddr_t target = (flags & MREMAP_FIXED) ? new_addr : 0;
    vaddr_t dst = mm_mmap_locked(mm, target, len, prot,
                           (target ? MAP_FIXED : 0) | MAP_ANONYMOUS |
                           ((src_vma->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE));
    if ((int64_t)dst < 0)
        return (int)dst;

    /* mm_mmap can merge the new anonymous VMA with neighbors; force exact
     * boundaries before turning it into a file-backed mapping. */
#ifdef CONFIG_NOMMU
    return -EINVAL; // Not supported on NOMMU
#else
    int sr = mm_split_vma_at(mm, dst);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, len);
        return sr;
    }
    sr = mm_split_vma_at(mm, dst + len);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, len);
        return sr;
    }
#endif

    vm_area_t *dst_vma = mm_find_vma(mm, dst);
    if (!dst_vma || dst_vma->start != dst || dst_vma->end != dst + len) {
        mm_munmap_locked(mm, dst, len);
        return -ENOMEM;
    }

    dst_vma->vm_flags = src_vma->vm_flags;
    dst_vma->pte_flags = src_vma->pte_flags;
    dst_vma->file_fd = src_vma->file_fd;
    dst_vma->file_offset = src_vma->file_offset + (src_addr - src_vma->start);
    dst_vma->file_vnode = src_vma->file_vnode;
    if (vma_ref_file(dst_vma) < 0) {
        dst_vma->file_fd = -1;
        if (dst_vma->file_vnode) {
            vnode_put(dst_vma->file_vnode);
            dst_vma->file_vnode = NULL;
        }
        mm_munmap_locked(mm, dst, len);
        return -EBADF;
    }

    for (vaddr_t off = 0; off < len; ) {
        int level = 0;
        vaddr_t base = 0;
        size_t leaf_size = 0;
        uint64_t src_va = src_addr + off;
        pte_t *src = pt_lookup_leaf(mm->pgdir, src_va, &level, &base, &leaf_size);
        if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src))
        {
            off += PAGE_SIZE;
            continue;
        }
        if (level > 0 && (base < src_addr || base + leaf_size > src_addr + len)) {
            int dr = mm_demote_huge_page(mm, src_va);
            if (dr < 0) {
                mm_munmap_locked(mm, dst, len);
                return dr;
            }
            continue;
        }

        paddr_t pa = arch_pte_addr(*src);
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn)) {
            mm_munmap_locked(mm, dst, len);
            return -ENOMEM;
        }

        int shared_file = src_vma &&
                          (src_vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED) &&
                          src_vma->file_vnode;
        page_cache_page_t *pcp = NULL;
        if (shared_file) {
            uint64_t idx = src_vma->file_offset + (src_va - src_vma->start);
            idx /= PAGE_SIZE;
            pcp = page_cache_get(src_vma->file_vnode, idx, 0);
            if (!pcp) {
                mm_munmap_locked(mm, dst, len);
                return -ENOMEM;
            }
        } else if (!shared_file) {
            frame_get(pfn);
        }
        int r = (level > 0) ? pt_map_huge(mm->pgdir, dst + off, pa, arch_pte_flags(*src))
                            : pt_map(mm->pgdir, dst + off, pa, arch_pte_flags(*src));
        if (r < 0) {
            if (shared_file && pcp) {
                page_cache_put(pcp);
            } else if (!shared_file) {
                frame_put(pfn);
            }
            mm_munmap_locked(mm, dst, len);
            return r;
        }
        mm->rss += leaf_size / PAGE_SIZE;
        off += leaf_size;
    }

    *out_addr = dst;
    return 0;
}

static __attribute__((unused)) int mm_move_mapping_pages(mm_struct_t *mm, vaddr_t old_addr,
                                 vaddr_t dst, size_t len, int dontunmap) {
    for (vaddr_t off = 0; off < len; ) {
        int level = 0;
        vaddr_t base = 0;
        size_t leaf_size = 0;
        uint64_t src_va = old_addr + off;
        pte_t *src = pt_lookup_leaf(mm->pgdir, src_va, &level, &base, &leaf_size);
        if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src))
        {
            off += PAGE_SIZE;
            continue;
        }
        if (level > 0 && (base < old_addr || base + leaf_size > old_addr + len ||
                          ((dst + off) & (leaf_size - 1)))) {
            int dr = mm_demote_huge_page(mm, src_va);
            if (dr < 0)
                return dr;
            continue;
        }

        paddr_t pa = arch_pte_addr(*src);
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn))
            return -ENOMEM;

        uint64_t pte_flags = arch_pte_flags(*src);
        vm_area_t *src_vma = mm_find_vma(mm, src_va);
        int shared_file = src_vma &&
                          (src_vma->vm_flags & (VM_FILE | VM_SHARED)) == (VM_FILE | VM_SHARED) &&
                          src_vma->file_vnode;
        page_cache_page_t *pcp = NULL;
        if (shared_file) {
            uint64_t idx = src_vma->file_offset + (src_va - src_vma->start);
            idx /= PAGE_SIZE;
            pcp = page_cache_get(src_vma->file_vnode, idx, 0);
            if (!pcp)
                return -ENOMEM;
        } else {
            frame_get(pfn);
        }
        int r = (level > 0) ? pt_map_huge(mm->pgdir, dst + off, pa, pte_flags)
                            : pt_map(mm->pgdir, dst + off, pa, pte_flags);
        if (r < 0) {
            if (shared_file) {
                page_cache_put(pcp);
            } else {
                frame_put(pfn);
            }
            return r;
        }

        if (!dontunmap &&
            pt_unmap_leaf(mm->pgdir, src_va, NULL, NULL, NULL, NULL) == 0)
            mm_tlb_note_change(mm, base, leaf_size);
        if (shared_file) {
            if (!dontunmap)
                page_cache_put(pcp);
        } else {
            if (!dontunmap)
                frame_put(pfn);
        }
        off += leaf_size;
    }
    return 0;
}

int mm_mremap_locked(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
                    size_t new_size, int flags, vaddr_t new_addr,
                    vaddr_t *out_addr) {
    if (!mm || !out_addr) return -EINVAL;
    if (new_size == 0) return -EINVAL;
    if (old_addr & (PAGE_SIZE - 1)) return -EINVAL;
    if (flags & ~(MREMAP_MAYMOVE | MREMAP_FIXED | MREMAP_DONTUNMAP))
        return -EINVAL;
    if ((flags & MREMAP_FIXED) &&
        (!(flags & MREMAP_MAYMOVE) || (new_addr & (PAGE_SIZE - 1))))
        return -EINVAL;
    if ((flags & MREMAP_DONTUNMAP) && !(flags & MREMAP_MAYMOVE))
        return -EINVAL;

    size_t old_len = ROUND_UP(old_size, PAGE_SIZE);
    size_t new_len = ROUND_UP(new_size, PAGE_SIZE);
    if (new_len == 0) return -EINVAL;
    if (old_addr + old_len < old_addr || old_addr + new_len < old_addr)
        return -EINVAL;
    if ((flags & MREMAP_FIXED) && new_addr + new_len < new_addr)
        return -EINVAL;

    vm_area_t *vma = mm_find_vma(mm, old_addr);
    if (!vma) return -EFAULT;

    if (old_size == 0) {
        if (!(flags & MREMAP_MAYMOVE) || (flags & MREMAP_DONTUNMAP))
            return -EINVAL;
        if ((flags & MREMAP_FIXED) &&
            new_addr < old_addr + new_len && old_addr < new_addr + new_len)
            return -EINVAL;
        return mm_clone_shared_mapping(mm, vma, old_addr, new_len, flags,
                                       new_addr, out_addr);
    }

    if (old_len == 0) return -EINVAL;
    if (old_addr + old_len < old_addr || old_addr + old_len > vma->end)
        return -EFAULT;
    if ((flags & MREMAP_DONTUNMAP) && old_len != new_len)
        return -EINVAL;

    int r = mm_split_vma_at(mm, old_addr);
    if (r < 0) return r;
    r = mm_split_vma_at(mm, old_addr + old_len);
    if (r < 0) return r;
    vma = mm_find_vma(mm, old_addr);
    if (!vma || vma->start != old_addr || vma->end < old_addr + old_len)
        return -EFAULT;

    if (new_len <= old_len && !(flags & (MREMAP_DONTUNMAP | MREMAP_FIXED))) {
        if (new_len < old_len)
            mm_munmap_locked(mm, old_addr + new_len, old_len - new_len);
        *out_addr = old_addr;
        return 0;
    }

    if (new_len > old_len && !(flags & MREMAP_DONTUNMAP)) {
        vaddr_t new_end = old_addr + new_len;
        int can_grow = !(flags & MREMAP_FIXED);
        if (old_addr + old_len != vma->end)
            can_grow = 0;
        if (can_grow && !mm_range_overlaps(mm, old_addr + old_len,
                                           new_len - old_len, vma)) {
            vma->end = new_end;
            mm->total_vm += (new_len - old_len) / PAGE_SIZE;
            vma_try_merge(mm, vma);
            *out_addr = old_addr;
            return 0;
        }
    }

    if (!(flags & MREMAP_MAYMOVE))
        return -ENOMEM;

    if ((flags & MREMAP_FIXED) &&
        new_addr < old_addr + old_len && old_addr < new_addr + new_len)
        return -EINVAL;

    int prot = mm_pte_flags_to_prot(vma->pte_flags);
    vaddr_t target = (flags & MREMAP_FIXED) ? new_addr : 0;
    vaddr_t dst = mm_mmap_locked(mm, target, new_len, prot,
                           (target ? MAP_FIXED : 0) | MAP_ANONYMOUS |
                           ((vma->vm_flags & VM_SHARED) ? MAP_SHARED : MAP_PRIVATE));
    if ((int64_t)dst < 0)
        return (int)dst;

    /* mm_mmap can merge the new anonymous VMA with neighbors; force exact
     * boundaries before turning it into a file-backed mapping. */
    int sr = mm_split_vma_at(mm, dst);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return sr;
    }
    sr = mm_split_vma_at(mm, dst + new_len);
    if (sr < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return sr;
    }

    vm_area_t *dst_vma = mm_find_vma(mm, dst);
    if (!dst_vma || dst_vma->start != dst || dst_vma->end != dst + new_len) {
        mm_munmap_locked(mm, dst, new_len);
        return -ENOMEM;
    }

    dst_vma->vm_flags = vma->vm_flags;
    dst_vma->pte_flags = vma->pte_flags;
    dst_vma->file_fd = vma->file_fd;
    dst_vma->file_offset = vma->file_offset + (old_addr - vma->start);
    dst_vma->file_vnode = vma->file_vnode;
    if (vma_ref_file(dst_vma) < 0) {
        dst_vma->file_fd = -1;
        if (dst_vma->file_vnode) {
            vnode_put(dst_vma->file_vnode);
            dst_vma->file_vnode = NULL;
        }
        mm_munmap_locked(mm, dst, new_len);
        return -EBADF;
    }

    size_t move_len = old_len < new_len ? old_len : new_len;
#ifdef CONFIG_NOMMU
    memcpy((void *)dst, (void *)old_addr, move_len);
    r = 0;
#else
    r = mm_move_mapping_pages(mm, old_addr, dst, move_len,
                              (flags & MREMAP_DONTUNMAP) != 0);
#endif
    if (r < 0) {
        mm_munmap_locked(mm, dst, new_len);
        return r;
    }

    if (!(flags & MREMAP_DONTUNMAP))
        mm_munmap_locked(mm, old_addr, old_len);

    *out_addr = dst;
    return 0;
}

int mm_mremap(mm_struct_t *mm, vaddr_t old_addr, size_t old_size,
              size_t new_size, int flags, vaddr_t new_addr,
              vaddr_t *out_addr)
{
    if (!mm) return -EINVAL;
    mm_tlb_invalidate_begin(mm);
    uint64_t flags_l = spin_lock_irqsave(&mm->lock);
    int r = mm_mremap_locked(mm, old_addr, old_size, new_size, flags,
                             new_addr, out_addr);
    spin_unlock_irqrestore(&mm->lock, flags_l);
    mm_tlb_invalidate_finish(mm);
    return r;
}

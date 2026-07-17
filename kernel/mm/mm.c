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
#include "mm/swap.h"

static inline int pte_user_readable(pte_t pte) {
    return arch_pte_is_leaf(pte) && (pte & PTE_U) && (pte & PTE_R);
}

static inline int pte_user_writable(pte_t pte) {
    return arch_pte_is_leaf(pte) && (pte & PTE_U) && (pte & PTE_W);
}

static inline size_t pt_level_size(int level) {
    return PAGE_SIZE << (ARCH_PT_BITS * level);
}

// 内存管理初始化函数
void mm_init(void) {
    extern char _bss_end[];
    printf("[MM] mm_init begin\n");
    pfa_init(va_to_pa(_bss_end)); // Buddy 物理页分配器
    printf("[MM] pfa_init done\n");
    slab_init(); // Slab 对象分配器
    printf("[MM] slab_init done\n");
    printf("[MM] Buddy+Slab: %d frames, %d free (%d MB)\n",
           (int)pfa.total_frames, (int)pfa.free_frames,
           (int)(pfa.free_frames * PAGE_SIZE / 1024 / 1024));
}

// 分配一个物理帧并清零
void *frame_alloc(void) {
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) return NULL;
    void *p = pfn_to_virt(pfn);
    memset(p, 0, PAGE_SIZE);
    return p;
}

// 分配一个物理帧，不清零（用于调用者会立即覆写的场景）
void *frame_alloc_nz(void) {
    pfn_t pfn = pfa_alloc_page();
    if (pfn == PFN_NONE) return NULL;
    return pfn_to_virt(pfn);
}

// 释放一个物理帧
void frame_free(void *addr) {
    if (!addr) return;
    pfn_t pfn = virt_to_pfn(addr);
    if (pfn_valid(pfn))
        pfa_free_page(pfn);
}

// 查询空闲物理帧数量
size_t frame_free_count(void) {
    return pfa_free_count();
}

#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)

// 创建一个新的页表
pte_t *pt_create(void) {
    pfn_t pfn = pfa_alloc(ARCH_PT_ROOT_ORDER);
    if (pfn == PFN_NONE)
        return NULL;
    pte_t *root = (pte_t *)pfn_to_virt(pfn);
    memset(root, 0, PAGE_SIZE << ARCH_PT_ROOT_ORDER);
    return root;
}

static void pt_free_table(pte_t *table, int level) {
    if (!table)
        return;
    pfn_t pfn = virt_to_pfn(table);
    if (!pfn_valid(pfn))
        return;
    pfa_free(pfn, level == ARCH_PT_ROOT_LEVEL ? ARCH_PT_ROOT_ORDER : 0);
}

static void pt_destroy_level(pte_t *table, int level) {
    if (!table) return;
    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        uint64_t pte = table[i];
        if ((pte & PTE_V) && !arch_pte_is_leaf(pte)) {
            paddr_t next_pa = arch_pte_addr(pte);
            pfn_t next_pfn = phys_to_pfn(next_pa);
            if ((next_pa & (PAGE_SIZE - 1)) || !pfn_valid(next_pfn)) {
                kerr("pt_destroy: skip invalid non-leaf pte[%d]=0x%lx pa=0x%lx\n",
                     i, (unsigned long)pte, (unsigned long)next_pa);
                table[i] = 0;
                continue;
            }
            pte_t *next = arch_pte_to_ptr(pte);
            pt_destroy_level(next, level - 1);
            table[i] = 0;
        }
    }
    pt_free_table(table, level);
}

// 递归销毁页表及其子页表
void pt_destroy(pt_root_t *pgdir) {
    pt_destroy_level(pgdir, ARCH_PT_ROOT_LEVEL);
}

// 遍历页表结构，查找或创建指定虚拟地址对应的 PTE
pte_t *pt_walk(pt_root_t *pgdir, vaddr_t va, int alloc) {
    pte_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 0; level--) {
        int vpn = arch_pt_vpn(va, level);
        pte_t pte = table[vpn];
#ifdef CONFIG_SWAP
        if (pte_is_swap(pte))
            return &table[vpn];
#endif
        if (pte & PTE_V) {
            if (arch_pte_is_leaf(pte))
                return NULL;
            table = arch_pte_to_ptr(pte);
        } else {
            if (!alloc) return NULL;
            pte_t *next = (pte_t *)frame_alloc();
            if (!next) return NULL;
            table[vpn] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
            table = next;
        }
    }
    return &table[arch_pt_vpn(va, 0)];
}

pte_t *pt_lookup_leaf(pt_root_t *pgdir, vaddr_t va, int *level_out,
                      vaddr_t *base_out, size_t *size_out) {
    if (!pgdir) return NULL;
    pte_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
        int idx = arch_pt_vpn(va, level);
        pte_t *pte = &table[idx];
#ifdef CONFIG_SWAP
        if (pte_is_swap(*pte)) {
            if (level_out) *level_out = 0;
            if (base_out) *base_out = va & ~(vaddr_t)(PAGE_SIZE - 1);
            if (size_out) *size_out = PAGE_SIZE;
            return pte;
        }
#endif
        if (!(*pte & PTE_V))
            return NULL;
        if (arch_pte_is_leaf(*pte)) {
            size_t sz = pt_level_size(level);
            if (level_out) *level_out = level;
            if (base_out) *base_out = va & ~(vaddr_t)(sz - 1);
            if (size_out) *size_out = sz;
            return pte;
        }
        if (level == 0)
            return NULL;
        table = arch_pte_to_ptr(*pte);
    }
    return NULL;
}

int mm_query_leaf(pt_root_t *pgdir, vaddr_t va, mm_leaf_info_t *out) {
    if (!out)
        return 0;
    memset(out, 0, sizeof(*out));
    if (!pgdir)
        return 0;

    int level = 0;
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(pgdir, va, &level, &base, &size);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte))
        return 0;

    out->level = level;
    out->base = base;
    out->size = size;
    out->pa = arch_pte_addr(*pte) + (va - base);
    out->flags = arch_pte_flags(*pte);
    out->dirty = (*pte & PTE_D) != 0;
    return 1;
}

uint64_t mm_pagemap_entry(pt_root_t *pgdir, vaddr_t va) {
    mm_leaf_info_t info;
    if (!mm_query_leaf(pgdir, va, &info))
        return 0;
    uint64_t pfn = info.pa / PAGE_SIZE;
    return (1ULL << 63) | (pfn & 0x7FFFFFFFFFFFFULL);
}

int mm_query_leaf_kaddr(pt_root_t *pgdir, vaddr_t va, void **kaddr_out,
                        size_t *avail_out) {
    if (!kaddr_out || !avail_out)
        return 0;
    *kaddr_out = NULL;
    *avail_out = 0;

    mm_leaf_info_t info;
    if (!mm_query_leaf(pgdir, va, &info))
        return 0;
    *kaddr_out = (void *)(info.pa + PAGE_OFFSET);
    *avail_out = info.size - (va - info.base);
    return 1;
}

int mm_fetch_user_insn32(pt_root_t *pgdir, vaddr_t va, uint32_t *out) {
    if (!out)
        return 0;
    void *kaddr = NULL;
    size_t avail = 0;
    if (!mm_query_leaf_kaddr(pgdir, va, &kaddr, &avail) || avail < sizeof(uint32_t))
        return 0;
    *out = *(uint32_t *)kaddr;
    return 1;
}

int mm_mark_leaf_dirty_if_writable(pt_root_t *pgdir, vaddr_t va) {
    if (!pgdir)
        return -1;
    pte_t *pte = pt_lookup_leaf(pgdir, va, NULL, NULL, NULL);
    if (!pte || !(*pte & PTE_V) || !(*pte & PTE_W))
        return -1;
    pte_t flags = arch_pte_flags(*pte) | PTE_D;
    *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
    arch_tlb_flush_page(va);
    return 0;
}

int mm_debug_pte_value(pt_root_t *pgdir, vaddr_t va, uintptr_t *slot_out,
                       pte_t *value_out) {
    if (slot_out)
        *slot_out = 0;
    if (value_out)
        *value_out = 0;
    if (!pgdir)
        return 0;
    pte_t *pte = pt_lookup_leaf(pgdir, va, NULL, NULL, NULL);
    if (slot_out)
        *slot_out = (uintptr_t)pte;
    if (value_out)
        *value_out = pte ? *pte : 0;
    return pte != NULL;
}

// 建立虚拟地址到物理地址的映射
int pt_map(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags) {
    pte_t *pte = pt_walk(pgdir, va, 1);
    if (!pte) return -ENOMEM;
    if (*pte & PTE_V) {
        paddr_t old_pa = arch_pte_addr(*pte);
        if (old_pa != pa) {
            int is_leaf = arch_pte_is_leaf(*pte);
            if (is_leaf)
                frame_put(phys_to_pfn(old_pa));
        }
    }
    /* Executable mappings may be populated through PAGE_OFFSET before being
     * installed at their user VA.  Synchronize RAM-backed code before the PTE
     * becomes visible so demand paging, fork/COW, VMO maps and ELF loading all
     * obey the same AArch64 I/D-cache contract. */
    if (flags & PTE_X) {
        pfn_t pfn = phys_to_pfn(pa);
        if (pfn_valid(pfn))
            arch_flush_icache_range(pfn_to_virt(pfn), PAGE_SIZE);
    }
    *pte = arch_pte_leaf(pa, flags);
    return 0;
}

int pt_map_huge(pt_root_t *pgdir, vaddr_t va, paddr_t pa, pte_t flags) {
    if (!pgdir) return -EINVAL;
    if ((va & (PMD_SIZE - 1)) || (pa & (PMD_SIZE - 1)))
        return -EINVAL;

    pte_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 1; level--) {
        int idx = arch_pt_vpn(va, level);
        pte_t pte = table[idx];
        if (pte & PTE_V) {
            if (arch_pte_is_leaf(pte))
                return -EEXIST;
            table = arch_pte_to_ptr(pte);
        } else {
            pte_t *next = (pte_t *)frame_alloc();
            if (!next) return -ENOMEM;
            table[idx] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
            table = next;
        }
    }

    pte_t *pte = &table[arch_pt_vpn(va, 1)];
    if (*pte & PTE_V)
        return -EEXIST;
#ifdef ARCH_HAS_PTE_BLOCK
    if (flags & PTE_X) {
        pfn_t pfn = phys_to_pfn(pa);
        if (pfn_valid(pfn))
            arch_flush_icache_range(pfn_to_virt(pfn), PMD_SIZE);
    }
#endif
#ifdef ARCH_HAS_PTE_BLOCK
    *pte = arch_pte_block(pa, flags);
#else
    *pte = arch_pte_leaf(pa, flags);
#endif
    return 0;
}

static int pt_table_empty(pte_t *table, int level) {
    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        if ((table[i] & PTE_V)
#ifdef CONFIG_SWAP
            || pte_is_swap(table[i])
#endif
        )
            return 0;
    }
    return 1;
}

// 取消虚拟地址的映射，并回收变空的中间页表页
int pt_unmap(pt_root_t *pgdir, vaddr_t va) {
    pte_t *path[ARCH_PT_ROOT_LEVEL + 1];
    int idx_path[ARCH_PT_ROOT_LEVEL + 1];
    pte_t *table = pgdir;

    path[ARCH_PT_ROOT_LEVEL] = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 0; level--) {
        int idx = arch_pt_vpn(va, level);
        idx_path[level] = idx;
        pte_t pte = table[idx];
        if (!(pte & PTE_V) || arch_pte_is_leaf(pte))
            return -EINVAL;
        table = arch_pte_to_ptr(pte);
        path[level - 1] = table;
    }

    int leaf_idx = arch_pt_vpn(va, 0);
    pte_t *pte = &table[leaf_idx];
    if (!(*pte & PTE_V) || !arch_pte_is_leaf(*pte))
        return -EINVAL;
    *pte = 0;

    for (int level = 0; level < ARCH_PT_ROOT_LEVEL; level++) {
        pte_t *child = path[level];
        pte_t *parent = path[level + 1];
        if (!pt_table_empty(child, level))
            break;
        parent[idx_path[level + 1]] = 0;
        frame_free(child);
    }
    return 0;
}

int pt_unmap_leaf(pt_root_t *pgdir, vaddr_t va, paddr_t *pa_out,
                  vaddr_t *base_out, size_t *size_out, int *level_out) {
    if (!pgdir) return -EINVAL;
    pte_t *path[ARCH_PT_ROOT_LEVEL + 1];
    int idx_path[ARCH_PT_ROOT_LEVEL + 1];
    pte_t *table = pgdir;

    path[ARCH_PT_ROOT_LEVEL] = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level >= 0; level--) {
        int idx = arch_pt_vpn(va, level);
        idx_path[level] = idx;
        pte_t *pte = &table[idx];
#ifdef CONFIG_SWAP
        if (pte_is_swap(*pte)) {
            if (level != 0)
                return -EINVAL;
            swap_free(pte_to_swp_entry(*pte));
            *pte = 0;
            if (pa_out) *pa_out = 0;
            if (base_out) *base_out = va & ~(vaddr_t)(PAGE_SIZE - 1);
            if (size_out) *size_out = PAGE_SIZE;
            if (level_out) *level_out = 0;
            return 0;
        }
#endif
        if (!(*pte & PTE_V))
            return -EINVAL;
        if (arch_pte_is_leaf(*pte)) {
            size_t sz = pt_level_size(level);
            vaddr_t base = va & ~(vaddr_t)(sz - 1);
            paddr_t pa = arch_pte_addr(*pte);
            *pte = 0;

            for (int l = level; l < ARCH_PT_ROOT_LEVEL; l++) {
                pte_t *child = path[l];
                pte_t *parent = path[l + 1];
                if (!pt_table_empty(child, l))
                    break;
                parent[idx_path[l + 1]] = 0;
                frame_free(child);
            }

            if (pa_out) *pa_out = pa;
            if (base_out) *base_out = base;
            if (size_out) *size_out = sz;
            if (level_out) *level_out = level;
            return 0;
        }
        if (level == 0)
            return -EINVAL;
        table = arch_pte_to_ptr(*pte);
        path[level - 1] = table;
    }
    return -EINVAL;
}

// 将虚拟地址转换为物理地址
paddr_t pt_translate(pt_root_t *pgdir, vaddr_t va) {
    vaddr_t base = 0;
    size_t size = 0;
    pte_t *pte = pt_lookup_leaf(pgdir, va, NULL, &base, &size);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) return 0;
    return arch_pte_addr(*pte) + (va - base);
}

// 将内核空间映射复制到新页表（内核空间共享）
// LoongArch 的内核空间是通过 DMW 直接翻译的，完全绕过了 TLB 和多级页表机制
// 可以置空来节省开销
void pt_map_kernel(pt_root_t *pgdir) {
    for (int i = ARCH_PT_USER_END; i < ARCH_PT_ENTRIES; i++) {
        if (boot_pgdir[i] & PTE_V)
            pgdir[i] = boot_pgdir[i];
    }
}

// 批量映射一段连续的虚拟地址范围
int pt_map_range(pt_root_t *pgdir, vaddr_t va, paddr_t pa, size_t size, pte_t flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        int r = pt_map(pgdir, va + off, pa + off, flags);
        if (r < 0) return r;
    }
    return 0;
}

// 递归克隆指定层级的页表项
static pte_t *pt_clone_level(pte_t *src, int level) {
    pte_t *dst = level == ARCH_PT_ROOT_LEVEL ?
        pt_create() : (pte_t *)frame_alloc();
    if (!dst) return NULL;

    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        pte_t pte = src[i];
        if (!(pte & PTE_V)) continue;

        int is_leaf = arch_pte_is_leaf(pte);

        if (is_leaf) {
            if (pte & PTE_U) {
                size_t leaf_size = pt_level_size(level);
                int order = (leaf_size == PMD_SIZE) ? PMD_ORDER : 0;
                if (leaf_size != PAGE_SIZE && leaf_size != PMD_SIZE) {
                    pt_destroy(dst);
                    return NULL;
                }
                pfn_t nf = pfa_alloc(order);
                if (nf == PFN_NONE) { pt_destroy(dst); return NULL; }
                memcpy(pfn_to_virt(nf), arch_pte_to_ptr(pte), leaf_size);
                dst[i] = arch_pte_leaf(pfn_to_phys(nf), arch_pte_flags(pte));
            } else {
                dst[i] = pte;
            }
        } else {
            pte_t *next_src = arch_pte_to_ptr(pte);
            pte_t *next_dst = pt_clone_level(next_src, level - 1);
            if (!next_dst) { pt_destroy(dst); return NULL; }
            dst[i] = arch_pte_from_pa(va_to_pa(next_dst)) | PTE_DIR;
        }
    }
    return dst;
}

// 克隆整个页表（从根节点开始）
pte_t *pt_clone(pt_root_t *src_pgdir) {
    if (!src_pgdir) return NULL;
    return pt_clone_level(src_pgdir, ARCH_PT_ROOT_LEVEL);
}

// 递归销毁用户空间的页表项（不释放内核共享部分）
static void pt_destroy_user_recursive(pte_t *table, int level) {
    if (!table) return;
    /* Only user half (0..255) lives at root; kernel half is shared
     * and must not be freed.  Lower levels may span all 512 entries. */
    int limit = (level == ARCH_PT_ROOT_LEVEL) ?
        ARCH_PT_USER_END : arch_pt_level_entries(level);
    for (int i = 0; i < limit; i++) {
        pte_t pte = table[i];
#ifdef CONFIG_SWAP
        if (pte_is_swap(pte)) {
            swap_free(pte_to_swp_entry(pte));
            table[i] = 0;
            continue;
        }
#endif
        if (!(pte & PTE_V)) continue;

        int is_leaf = arch_pte_is_leaf(pte);

        if (is_leaf) {
            if (pte & PTE_U)
                frame_put(phys_to_pfn(arch_pte_addr(pte)));
            table[i] = 0;
        } else {
            paddr_t next_pa = arch_pte_addr(pte);
            pfn_t next_pfn = phys_to_pfn(next_pa);
            if ((next_pa & (PAGE_SIZE - 1)) || !pfn_valid(next_pfn)) {
                kerr("pt_destroy_user: skip invalid non-leaf level=%d idx=%d pte=0x%lx pa=0x%lx\n",
                     level, i, (unsigned long)pte, (unsigned long)next_pa);
                table[i] = 0;
                continue;
            }
            pte_t *next = arch_pte_to_ptr(pte);
            pt_destroy_user_recursive(next, level - 1);
            pt_free_table(next, level - 1);
            table[i] = 0;
        }
    }
}

// 销毁用户空间页表
void pt_destroy_user(pt_root_t *pgdir) {
    if (!pgdir) return;
    pt_destroy_user_recursive(pgdir, ARCH_PT_ROOT_LEVEL);
    pt_free_table(pgdir, ARCH_PT_ROOT_LEVEL);
}

#endif /* defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU) */

static inline int user_range_ok(uint64_t va, size_t n) {
    va = (uint64_t)(vaddr_t)va;
    if (n == 0)
        return 1;
    if (va >= USER_VA_LIMIT)
        return 0;
    return n <= USER_VA_LIMIT - va;
}

static int user_resolve_leaf(task_t *t, uint64_t va, int write,
                             void **kaddr_out, size_t *avail_out) {
    va = (uint64_t)(vaddr_t)va;
#ifdef CONFIG_NOMMU
    (void)t; (void)write;
    *kaddr_out = (void *)va;
    *avail_out = USER_VA_LIMIT > va ? USER_VA_LIMIT - va : 0;
    return 0;
#else
    vaddr_t leaf_base = 0;
    size_t leaf_size = 0;
    pte_t *pte = pt_lookup_leaf(t->pgdir, va, NULL, &leaf_base, &leaf_size);
    if (!pte || !(*pte & PTE_V)) {
        int r = handle_demand_fault(t, va);
        if (r < 0)
            return -EFAULT;
        pte = pt_lookup_leaf(t->pgdir, va, NULL, &leaf_base, &leaf_size);
        if (!pte || !(*pte & PTE_V))
            return -EFAULT;
    }

    if (write) {
        if (!arch_pte_is_leaf(*pte) || !(*pte & PTE_U))
            return -EFAULT;
        if (!(*pte & PTE_W)) {
            int r = handle_cow_fault(t, va);
            if (r < 0)
                return -EFAULT;
            pte = pt_lookup_leaf(t->pgdir, va, NULL, &leaf_base, &leaf_size);
            if (!pte || !(*pte & PTE_V))
                return -EFAULT;
        }
        if (!pte_user_writable(*pte))
            return -EFAULT;
    } else if (!pte_user_readable(*pte)) {
        return -EFAULT;
    }

    size_t page_off = va - leaf_base;
    paddr_t pa = arch_pte_addr(*pte);
    *kaddr_out = (void *)(pa + PAGE_OFFSET + page_off);
    *avail_out = leaf_size - page_off;
    return 0;
#endif
}

int user_buffer_segment(const void *user, size_t len, int write,
                        void **kaddr, size_t *chunk) {
    task_t *t = proc_current();
    if (!t || !t->mm || !kaddr || !chunk)
        return -EFAULT;
    if (len == 0) {
        *kaddr = NULL;
        *chunk = 0;
        return 0;
    }
    uint64_t va = (uint64_t)(vaddr_t)user;
    if (!user_range_ok(va, len))
        return -EFAULT;
    if (user_resolve_leaf(t, va, write, kaddr, chunk) < 0)
        return -EFAULT;
    if (*chunk > len)
        *chunk = len;
    return 0;
}

// 从用户空间拷贝数据到内核空间
long copy_from_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    if (!user_range_ok((uint64_t)src, n)) return -EFAULT;
    size_t copied = 0;
    while (copied < n) {
        void *kaddr;
        size_t chunk;
        if (user_resolve_leaf(t, (uint64_t)src + copied, 0, &kaddr, &chunk) < 0)
            return -EFAULT;
        if (chunk > n - copied)
            chunk = n - copied;
        memcpy((char *)dst + copied, kaddr, chunk);
        copied += chunk;
    }
    return (long)copied;
}

// 从内核空间拷贝数据到用户空间
long copy_to_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    if (!user_range_ok((uint64_t)dst, n)) return -EFAULT;
    size_t copied = 0;
    while (copied < n) {
        void *kaddr;
        size_t chunk;
        if (user_resolve_leaf(t, (uint64_t)dst + copied, 1, &kaddr, &chunk) < 0)
            return -EFAULT;
        if (chunk > n - copied)
            chunk = n - copied;
        memcpy(kaddr, (const char *)src + copied, chunk);
        copied += chunk;
    }
    return (long)copied;
}

// 从用户空间拷贝字符串到内核空间
long user_strncpy(char *dst, const char *src, size_t max) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    if (max == 0) return -EINVAL;
    size_t i = 0;
    while (i < max - 1) {
        uint64_t va = (uint64_t)(src + i);
        if (!user_range_ok(va, 1)) return -EFAULT;
        void *kaddr;
        size_t chunk;
        if (user_resolve_leaf(t, va, 0, &kaddr, &chunk) < 0)
            return -EFAULT;
        if (chunk > max - 1 - i) chunk = max - 1 - i;
        const char *src_page = (const char *)kaddr;
        for (size_t j = 0; j < chunk; j++) {
            dst[i + j] = src_page[j];
            if (src_page[j] == '\0') return (long)(i + j);
        }
        i += chunk;
    }
    dst[i] = '\0';
    return (long)i;
}

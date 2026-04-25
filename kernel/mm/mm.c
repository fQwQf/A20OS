#include "defs.h"
#include "mm.h"
#include "frame.h"
#include "slab.h"
#include "panic.h"
#include "stdio.h"
#include "string.h"
#include "klog.h"

// 虚拟地址转换为物理地址
static inline paddr_t va_to_pa(const void *va) {
    return (paddr_t)((uint64_t)(uintptr_t)va - PAGE_OFFSET);
}

static inline int pte_user_readable(uint64_t pte) {
    return arch_pte_is_leaf(pte) && (pte & PTE_U) && (pte & PTE_R);
}

static inline int pte_user_writable(uint64_t pte) {
    return arch_pte_is_leaf(pte) && (pte & PTE_U) && (pte & PTE_W);
}

// 内存管理初始化函数
void mm_init(void) {
    extern char _bss_end[];
    pfa_init(PHYS_MEMORY_BASE, PHYS_MEMORY_END,
             va_to_pa(_bss_end)); // Buddy 物理页分配器
    slab_init(); // Slab 对象分配器
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

// 创建一个新的页表
uint64_t *pt_create(void) {
    return (uint64_t *)frame_alloc();
}

// 递归销毁页表及其子页表
void pt_destroy(uint64_t *pgdir) {
    if (!pgdir) return;
    for (int i = 0; i < ARCH_PT_ENTRIES; i++) {
        uint64_t pte = pgdir[i];
        if ((pte & PTE_V) && !arch_pte_is_leaf(pte)) {
            paddr_t next_pa = arch_pte_addr(pte);
            pfn_t next_pfn = phys_to_pfn(next_pa);
            if ((next_pa & (PAGE_SIZE - 1)) || !pfn_valid(next_pfn)) {
                kerr("pt_destroy: skip invalid non-leaf pte[%d]=0x%lx pa=0x%lx\n",
                     i, (unsigned long)pte, (unsigned long)next_pa);
                pgdir[i] = 0;
                continue;
            }
            uint64_t *next = arch_pte_to_ptr(pte);
            pt_destroy(next);
            pgdir[i] = 0;
        }
    }
    frame_free(pgdir);
}

// 遍历页表结构，查找或创建指定虚拟地址对应的 PTE
uint64_t *pt_walk(uint64_t *pgdir, vaddr_t va, int alloc) {
    uint64_t *table = pgdir;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 0; level--) {
        int vpn = arch_pt_vpn(va, level);
        uint64_t pte = table[vpn];
        if (pte & PTE_V) {
            if (arch_pte_is_leaf(pte))
                return NULL;
            table = arch_pte_to_ptr(pte);
        } else {
            if (!alloc) return NULL;
            uint64_t *next = (uint64_t *)frame_alloc();
            if (!next) return NULL;
            table[vpn] = arch_pte_from_pa(va_to_pa(next)) | PTE_V;
            table = next;
        }
    }
    return &table[arch_pt_vpn(va, 0)];
}

// 建立虚拟地址到物理地址的映射
int pt_map(uint64_t *pgdir, vaddr_t va, paddr_t pa, uint64_t flags) {
    uint64_t *pte = pt_walk(pgdir, va, 1);
    if (!pte) return -ENOMEM;
    if (*pte & PTE_V) {
        paddr_t old_pa = arch_pte_addr(*pte);
        if (old_pa != pa) {
            int is_leaf = arch_pte_is_leaf(*pte);
            if (is_leaf)
                frame_put(phys_to_pfn(old_pa));
        }
    }
    *pte = arch_pte_leaf(pa, flags);
    return 0;
}

// 取消虚拟地址的映射
int pt_unmap(uint64_t *pgdir, vaddr_t va) {
    uint64_t *pte = pt_walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_V)) return -EINVAL;
    *pte = 0;
    return 0;
}

// 将虚拟地址转换为物理地址
paddr_t pt_translate(uint64_t *pgdir, vaddr_t va) {
    uint64_t *pte = pt_walk(pgdir, va, 0);
    if (!pte || !(*pte & PTE_V) || !arch_pte_is_leaf(*pte)) return 0;
    return arch_pte_addr(*pte) | (va & (PAGE_SIZE - 1));
}

// 将内核空间映射复制到新页表（内核空间共享）
// LoongArch 的内核空间是通过 DMW 直接翻译的，完全绕过了 TLB 和多级页表机制
// 可以置空来节省开销
void pt_map_kernel(uint64_t *pgdir) {
    for (int i = ARCH_PT_USER_END; i < ARCH_PT_ENTRIES; i++) {
        if (boot_pgdir[i] & PTE_V)
            pgdir[i] = boot_pgdir[i];
    }
}

// 批量映射一段连续的虚拟地址范围
int pt_map_range(uint64_t *pgdir, vaddr_t va, paddr_t pa, size_t size, uint64_t flags) {
    size = ROUND_UP(size, PAGE_SIZE);
    for (size_t off = 0; off < size; off += PAGE_SIZE) {
        int r = pt_map(pgdir, va + off, pa + off, flags);
        if (r < 0) return r;
    }
    return 0;
}

// 递归克隆指定层级的页表项
static uint64_t *pt_clone_level(uint64_t *src, int level) {
    uint64_t *dst = (uint64_t *)frame_alloc();
    if (!dst) return NULL;

    for (int i = 0; i < ARCH_PT_ENTRIES; i++) {
        uint64_t pte = src[i];
        if (!(pte & PTE_V)) continue;

        int is_leaf = arch_pte_is_leaf(pte);

        if (is_leaf) {
            if (pte & PTE_U) {
                void *nf = frame_alloc();
                if (!nf) { pt_destroy(dst); return NULL; }
                memcpy(nf, arch_pte_to_ptr(pte), PAGE_SIZE);
                dst[i] = arch_pte_from_pa(va_to_pa(nf)) | arch_pte_flags(pte);
            } else {
                dst[i] = pte;
            }
        } else {
            uint64_t *next_src = arch_pte_to_ptr(pte);
            uint64_t *next_dst = pt_clone_level(next_src, level - 1);
            if (!next_dst) { pt_destroy(dst); return NULL; }
            dst[i] = arch_pte_from_pa(va_to_pa(next_dst)) | PTE_V;
        }
    }
    return dst;
}

// 克隆整个页表（从根节点开始）
uint64_t *pt_clone(uint64_t *src_pgdir) {
    if (!src_pgdir) return NULL;
    return pt_clone_level(src_pgdir, ARCH_PT_ROOT_LEVEL);
}

// 递归销毁用户空间的页表项（不释放内核共享部分）
static void pt_destroy_user_recursive(uint64_t *table, int level) {
    if (!table) return;
    /* Only user half (0..255) lives at root; kernel half is shared
     * and must not be freed.  Lower levels may span all 512 entries. */
    int limit = (level == ARCH_PT_ROOT_LEVEL) ? ARCH_PT_USER_END : ARCH_PT_ENTRIES;
    for (int i = 0; i < limit; i++) {
        uint64_t pte = table[i];
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
            uint64_t *next = arch_pte_to_ptr(pte);
            pt_destroy_user_recursive(next, level - 1);
            frame_free(next);
            table[i] = 0;
        }
    }
}

// 销毁用户空间页表
void pt_destroy_user(uint64_t *pgdir) {
    if (!pgdir) return;
    pt_destroy_user_recursive(pgdir, ARCH_PT_ROOT_LEVEL);
    frame_free(pgdir);
}

#include "vm.h"
#include "proc.h"
#include "trap.h"

// 从用户空间拷贝数据到内核空间
long copy_from_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    size_t copied = 0;
    while (n > 0) {
        uint64_t va = (uint64_t)src + copied;
        if (va >= 0x4000000000UL) return -EFAULT;
        uint64_t *pte = pt_walk(t->pgdir, va, 0);
        if (!pte || !(*pte & PTE_V)) {
            // 处理缺页异常
            if (handle_demand_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V)) return -EFAULT;
        }
        if (!pte_user_readable(*pte)) return -EFAULT;
        paddr_t pa = arch_pte_addr(*pte);
        size_t page_off = va & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > n) chunk = n;
        memcpy((char*)dst + copied, (void*)(pa + PAGE_OFFSET + page_off), chunk);
        copied += chunk;
        n -= chunk;
    }
    return (long)copied;
}

// 从内核空间拷贝数据到用户空间
long copy_to_user(void *dst, const void *src, size_t n) {
    task_t *t = proc_current();
    if (!t || !t->mm) return -EFAULT;
    size_t copied = 0;
    while (n > 0) {
        uint64_t va = (uint64_t)dst + copied;
        if (va >= 0x4000000000UL) return -EFAULT;
        uint64_t *pte = pt_walk(t->pgdir, va, 0);
        if (!pte || !(*pte & PTE_V)) {
            // 处理缺页异常
            if (handle_demand_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V)) return -EFAULT;
        }
        if (!arch_pte_is_leaf(*pte) || !(*pte & PTE_U)) return -EFAULT;
        if (!(*pte & PTE_W)) {
            if (handle_cow_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V) || !pte_user_writable(*pte)) return -EFAULT;
        }
        paddr_t pa = arch_pte_addr(*pte);
        size_t page_off = va & (PAGE_SIZE - 1);
        size_t chunk = PAGE_SIZE - page_off;
        if (chunk > n) chunk = n;
        memcpy((void*)(pa + PAGE_OFFSET + page_off), (const char*)src + copied, chunk);
        copied += chunk;
        n -= chunk;
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
        if (va >= 0x4000000000UL) return -EFAULT;
        uint64_t *pte = pt_walk(t->pgdir, va, 0);
        if (!pte || !(*pte & PTE_V)) {
            // 处理缺页异常
            if (handle_demand_fault(t, va) < 0) return -EFAULT;
            pte = pt_walk(t->pgdir, va, 0);
            if (!pte || !(*pte & PTE_V)) return -EFAULT;
        }
        if (!pte_user_readable(*pte)) return -EFAULT;
        paddr_t pa = arch_pte_addr(*pte);
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

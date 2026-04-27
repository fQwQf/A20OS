#include "mm/vm.h"
#include "mm/mm.h"
#include "mm/frame.h"
#include "mm/slab.h"
#include "core/string.h"
#include "core/panic.h"
#include "core/klog.h"

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

static int mm_fork_clone_page(mm_struct_t *child, mm_struct_t *parent, uint64_t va) {
    uint64_t *src = pt_walk(parent->pgdir, va, 0);
    if (!src || !(*src & PTE_V) || !arch_pte_is_leaf(*src) || !(*src & PTE_U))
        return 0;

    uint64_t *dst = pt_walk(child->pgdir, va, 0);
    if (dst && (*dst & PTE_V))
        return 0;

    paddr_t pa = arch_pte_addr(*src);
    pfn_t pfn = phys_to_pfn(pa);
    if (!pfn_valid(pfn))
        return -ENOMEM;

    uint64_t flags = mm_cow_flags(*src);
    frame_get(pfn);

    int r = pt_map(child->pgdir, va, pa, flags);
    if (r < 0) {
        frame_put(pfn);
        return r;
    }

    if (*src & (PTE_W | PTE_COW))
        *src = arch_pte_leaf(pa, flags);
    child->rss++;
    return 0;
}

static int mm_fork_clone_range(mm_struct_t *child, mm_struct_t *parent,
                               uint64_t start, uint64_t end) {
    start = ROUND_DOWN(start, PAGE_SIZE);
    end = ROUND_UP(end, PAGE_SIZE);
    for (uint64_t va = start; va < end; va += PAGE_SIZE) {
        int r = mm_fork_clone_page(child, parent, va);
        if (r < 0)
            return r;
    }
    return 0;
}

// 创建一个新的内存描述符
mm_struct_t *mm_create(void) {
    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (!mm) return NULL;

    mm->pgdir = pt_create();
    if (!mm->pgdir) { kfree(mm); return NULL; }
    pt_map_kernel(mm->pgdir);  // 映射内核空间

    mm->mmap       = NULL;
    mm->brk        = 0;
    mm->start_brk  = 0;
    mm->mmap_base  = MMAP_BASE_ADDR;
    mm->stack_top  = 0;
    mm->stack_bottom = 0;
    mm->total_vm   = 0;
    mm->rss        = 0;
    mm->refcount   = 1;
    return mm;
}

// 释放 VMA 对应的物理页面
static void free_vma_pages(mm_struct_t *mm, vm_area_t *vma) {
    if (!mm->pgdir) return;
    for (uint64_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
        paddr_t pa = pt_translate(mm->pgdir, va);
        pt_unmap(mm->pgdir, va);
        if (pa) frame_put(phys_to_pfn(pa));
    }
}

// 销毁内存描述符及其所有资源
void mm_destroy(mm_struct_t *mm) {
    if (!mm) return;
    if (--mm->refcount > 0) return;  // 引用计数归零才真正销毁

    // 释放所有 VMA 及其物理页面
    vm_area_t *vma = mm->mmap;
    while (vma) {
        free_vma_pages(mm, vma);
        vm_area_t *next = vma->next;
        kfree(vma);
        vma = next;
    }

    if (mm->pgdir) pt_destroy_user(mm->pgdir);
    kfree(mm);
}

// 查找包含指定地址的 VMA
vm_area_t *mm_find_vma(mm_struct_t *mm, uint64_t addr) {
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (addr < v->end && addr >= v->start) return v;
        if (v->start > addr) break;
    }
    return NULL;
}

// 在虚拟地址空间中查找一个足够大的空隙
uint64_t mm_find_gap(mm_struct_t *mm, uint64_t hint, size_t len) {
    uint64_t prev_end = hint;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= prev_end && v->start - prev_end >= len) return prev_end;
        if (v->end > prev_end) prev_end = v->end;
    }
    return prev_end;
}

// 插入一个 VMA 到链表中，并尝试合并相邻的相同权限区域
void mm_insert_vma(mm_struct_t *mm, vm_area_t *newv) {
    vm_area_t **pp = &mm->mmap;
    vm_area_t *prev = NULL;
    while (*pp && (*pp)->start < newv->start) {
        prev = *pp;
        pp = &(*pp)->next;
    }
    newv->next = *pp;
    newv->prev = prev;
    if (*pp) (*pp)->prev = newv;
    *pp = newv;

    // 尝试与后一个 VMA 合并
    if (newv->next && newv->end == newv->next->start &&
        newv->vm_flags == newv->next->vm_flags &&
        newv->pte_flags == newv->next->pte_flags) {
        vm_area_t *nxt = newv->next;
        newv->end = nxt->end;
        newv->next = nxt->next;
        if (nxt->next) nxt->next->prev = newv;
        kfree(nxt);
    }
    // 尝试与前一个 VMA 合并
    if (newv->prev && newv->prev->end == newv->start &&
        newv->prev->vm_flags == newv->vm_flags &&
        newv->prev->pte_flags == newv->pte_flags) {
        vm_area_t *prv = newv->prev;
        prv->end = newv->end;
        prv->next = newv->next;
        if (newv->next) newv->next->prev = prv;
        kfree(newv);
    }
}

static int mm_split_vma_at(mm_struct_t *mm, uint64_t addr) {
    vm_area_t *v = mm_find_vma(mm, addr);
    if (!v || addr <= v->start || addr >= v->end)
        return 0;

    vm_area_t *tail = kcalloc(1, sizeof(vm_area_t));
    if (!tail)
        return -ENOMEM;

    *tail = *v;
    tail->start = addr;
    tail->prev = v;
    tail->next = v->next;
    if (tail->next)
        tail->next->prev = tail;

    v->end = addr;
    v->next = tail;
    return 0;
}

static vm_area_t *vma_split(vm_area_t *vma, uint64_t split) {
    if (!vma) return NULL;
    if (split <= vma->start || split >= vma->end) return vma;

    vm_area_t *tail = kcalloc(1, sizeof(vm_area_t));
    if (!tail) return NULL;

    *tail = *vma;
    tail->start = split;
    tail->prev = vma;
    tail->next = vma->next;
    if (tail->next) tail->next->prev = tail;

    vma->end = split;
    vma->next = tail;
    return tail;
}

static void vma_try_merge(vm_area_t *vma) {
    if (!vma) return;

    if (vma->prev &&
        vma->prev->end == vma->start &&
        vma->prev->vm_flags == vma->vm_flags &&
        vma->prev->pte_flags == vma->pte_flags) {
        vm_area_t *prev = vma->prev;
        prev->end = vma->end;
        prev->next = vma->next;
        if (vma->next) vma->next->prev = prev;
        kfree(vma);
        vma = prev;
    }

    if (vma->next &&
        vma->end == vma->next->start &&
        vma->next->vm_flags == vma->vm_flags &&
        vma->next->pte_flags == vma->pte_flags) {
        vm_area_t *next = vma->next;
        vma->end = next->end;
        vma->next = next->next;
        if (next->next) next->next->prev = vma;
        kfree(next);
    }
}

// 将保护标志转换为页表项标志
uint64_t prot_to_pte(int prot) {
    uint64_t f = PTE_V | PTE_U | PTE_A | PTE_MAT1 | PTE_LEAF;
    if (prot & 1) f |= PTE_R;
    if (prot & 2) f |= (PTE_W | PTE_D);
    if (prot & 4) f |= PTE_X;
    if (f & PTE_W) f |= PTE_R;
    return f;
}

// 创建内存映射（mmap 系统调用的实现）
uint64_t mm_mmap(mm_struct_t *mm, uint64_t addr, size_t len, int prot, int flags) {
    kdebug("[MMAP] req_addr=0x%lx len=%lu prot=%d -> ", addr, len, prot);

    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return (uint64_t)-EINVAL;

    uint64_t ptef = prot_to_pte(prot);
    uint64_t vmf = VM_ANON;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;

    // 处理 MAP_FIXED 标志
    if ((flags & MAP_FIXED) && addr != 0) {
        mm_munmap(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

    // 查找合适的虚拟地址
    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);

    if (addr == 0) return (uint64_t)-ENOMEM;

    // 创建新的 VMA
    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) return (uint64_t)-ENOMEM;
    vma->start     = addr;
    vma->end       = addr + len;
    vma->vm_flags  = vmf;
    vma->pte_flags = ptef;

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;

    kdebug("ret=0x%lx\n", addr);

    return addr;
}

// 取消内存映射（munmap 系统调用的实现）
int mm_munmap(mm_struct_t *mm, uint64_t addr, size_t len) {
    len = ROUND_UP(len, PAGE_SIZE);
    uint64_t end = addr + len;

    vm_area_t *vma = mm->mmap;
    while (vma) {
        vm_area_t *next = vma->next;
        if (vma->start >= end || vma->end <= addr) { vma = next; continue; }

        uint64_t clip_start = vma->start < addr ? addr : vma->start;
        uint64_t clip_end   = vma->end > end ? end : vma->end;

        // 释放该范围内的物理页面
        for (uint64_t va = clip_start; va < clip_end; va += PAGE_SIZE) {
            paddr_t pa = pt_translate(mm->pgdir, va);
            pt_unmap(mm->pgdir, va);
            if (pa) { frame_put(phys_to_pfn(pa)); mm->rss--; }
        }
        mm->total_vm -= (clip_end - clip_start) / PAGE_SIZE;

        // 根据取消映射的范围，对 VMA 进行删除或拆分
        if (addr <= vma->start && end >= vma->end) {
            // 完全删除 VMA
            if (vma->prev) vma->prev->next = vma->next;
            else mm->mmap = vma->next;
            if (vma->next) vma->next->prev = vma->prev;
            kfree(vma);
        } else if (addr <= vma->start) {
            // 从开头部分删除
            vma->start = clip_end;
        } else if (end >= vma->end) {
            // 从结尾部分删除
            vma->end = clip_start;
        } else {
            // 从中间删除，需要拆分成两个 VMA
            vm_area_t *tail = kcalloc(1, sizeof(vm_area_t));
            if (!tail) return -ENOMEM;
            *tail = *vma;
            tail->start = clip_end;
            tail->end = vma->end;
            tail->prev = vma;
            tail->next = vma->next;
            if (vma->next) vma->next->prev = tail;
            vma->next = tail;
            vma->end = clip_start;
        }
        vma = next;
    }
    arch_tlb_flush();  // 刷新 TLB
    return 0;
}

// 调整堆大小（brk 系统调用的实现）
uint64_t mm_brk(mm_struct_t *mm, uint64_t newbrk) {
    if (newbrk == 0) return mm->brk;
    if (newbrk < mm->start_brk) return mm->brk;
    if (newbrk < mm->brk) {
        // 缩小堆，释放多余的物理页面
        uint64_t new_brk_page = ROUND_UP(newbrk, PAGE_SIZE);
        uint64_t old_brk_page = ROUND_UP(mm->brk, PAGE_SIZE);
        for (uint64_t va = new_brk_page; va < old_brk_page; va += PAGE_SIZE) {
            uint64_t *pte = pt_walk(mm->pgdir, va, 0);
            if (pte && (*pte & PTE_V)) {
                paddr_t pa = arch_pte_addr(*pte);
                frame_put(phys_to_pfn(pa));
                mm->rss--;
                pt_unmap(mm->pgdir, va);
            }
        }
        if (new_brk_page < old_brk_page)
            arch_tlb_flush();  // 刷新 TLB
    }
    mm->brk = newbrk;
    return mm->brk;
}

// 修改内存区域的保护属性（mprotect 系统调用的实现）
int mm_mprotect(mm_struct_t *mm, uint64_t addr, size_t len, int prot) {
    if (!mm || !mm->pgdir) return -EINVAL;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return 0;
    if (addr & (PAGE_SIZE - 1)) return -EINVAL;

    uint64_t ptef = prot_to_pte(prot);
    uint64_t vm_prot = 0;
    if (prot & 1) vm_prot |= VM_READ;
    if (prot & 2) vm_prot |= VM_WRITE;
    if (prot & 4) vm_prot |= VM_EXEC;
    uint64_t end = addr + len;
    int touched = 0;

    int r = mm_split_vma_at(mm, addr);
    if (r < 0) return r;
    r = mm_split_vma_at(mm, end);
    if (r < 0) return r;

    for (vm_area_t *v = mm_find_vma(mm, addr); v && v->start < end; ) {
        vm_area_t *next = v->next;
        uint64_t s = v->start < addr ? addr : v->start;
        uint64_t e = v->end > end ? end : v->end;

        if (s > v->start) {
            v = vma_split(v, s);
            if (!v) return -ENOMEM;
            next = v->next;
        }
        if (e < v->end) {
            if (!vma_split(v, e)) return -ENOMEM;
            next = v->next;
        }

        for (uint64_t va = v->start; va < v->end; va += PAGE_SIZE) {
            uint64_t *pte = pt_walk(mm->pgdir, va, 0);
            if (pte && (*pte & PTE_V)) {
                uint64_t flags = *pte & (PTE_R | PTE_W | PTE_X | PTE_U |
                                         PTE_G | PTE_A | PTE_D | PTE_COW |
                                         PTE_LEAF | PTE_MAT1);
                flags &= ~(uint64_t)(PTE_R | PTE_W | PTE_X | PTE_D);
                flags |= ptef & (PTE_R | PTE_W | PTE_X | PTE_D);
                *pte = arch_pte_leaf(arch_pte_addr(*pte), flags);
            }
        }
        v->pte_flags = (v->pte_flags & ~(uint64_t)(PTE_R | PTE_W | PTE_X | PTE_D)) |
                       (ptef & (PTE_R | PTE_W | PTE_X | PTE_D));
        v->vm_flags  = (v->vm_flags & ~(uint64_t)(VM_READ | VM_WRITE | VM_EXEC)) |
                       vm_prot;
        vma_try_merge(v);
        touched = 1;
        v = next;
    }

    if (touched)
        arch_tlb_flush();  // 刷新 TLB
    return 0;
}

// 创建子进程的内存空间（fork 时使用，实现写时复制）
mm_struct_t *mm_fork(mm_struct_t *parent) {
    if (!parent) return NULL;
    mm_struct_t *child = kcalloc(1, sizeof(mm_struct_t));
    if (!child) return NULL;
    *child = *parent;
    child->refcount = 1;
    child->rss = parent->rss;
    child->mmap = NULL;

    child->pgdir = pt_create();
    if (!child->pgdir) { kfree(child); return NULL; }
    pt_map_kernel(child->pgdir);

    // 复制所有 VMA
    vm_area_t **tail = &child->mmap;
    vm_area_t *prev = NULL;
    for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
        kdebug("VMA: pv=%p\n", pv);
        vm_area_t *cv = kcalloc(1, sizeof(vm_area_t));
        if (!cv) goto fail;
        *cv = *pv;
        cv->prev = prev;
        cv->next = NULL;
        *tail = cv;
        tail = &cv->next;
        prev = cv;
    }

    for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
        if (mm_fork_clone_range(child, parent, pv->start, pv->end) < 0)
            goto fail;
    }

    if (parent->start_brk < parent->brk) {
        if (mm_fork_clone_range(child, parent, parent->start_brk, parent->brk) < 0)
            goto fail;
    }

    if (parent->stack_bottom && parent->stack_top) {
        if (mm_fork_clone_range(child, parent, parent->stack_bottom,
                                parent->stack_top) < 0)
            goto fail;
    }

    arch_tlb_flush();  // 刷新 TLB
    return child;
fail:
    mm_destroy(child);
    return NULL;
}

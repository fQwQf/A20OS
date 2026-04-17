#include "vm.h"
#include "mm.h"
#include "frame.h"
#include "slab.h"
#include "string.h"
#include "panic.h"
#include "klog.h"

static inline paddr_t va_to_pa(const void *va) {
    return (paddr_t)((uint64_t)(uintptr_t)va - PAGE_OFFSET);
}

mm_struct_t *mm_create(void) {
    mm_struct_t *mm = kcalloc(1, sizeof(mm_struct_t));
    if (!mm) return NULL;

    mm->pgdir = pt_create();
    if (!mm->pgdir) { kfree(mm); return NULL; }
    pt_map_kernel(mm->pgdir);

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

static void free_vma_pages(mm_struct_t *mm, vm_area_t *vma) {
    if (!mm->pgdir) return;
    for (uint64_t va = vma->start; va < vma->end; va += PAGE_SIZE) {
        paddr_t pa = pt_translate(mm->pgdir, va);
        pt_unmap(mm->pgdir, va);
        if (pa) frame_put(phys_to_pfn(pa));
    }
}

void mm_destroy(mm_struct_t *mm) {
    if (!mm) return;
    if (--mm->refcount > 0) return;

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

vm_area_t *mm_find_vma(mm_struct_t *mm, uint64_t addr) {
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (addr < v->end && addr >= v->start) return v;
        if (v->start > addr) break;
    }
    return NULL;
}

uint64_t mm_find_gap(mm_struct_t *mm, uint64_t hint, size_t len) {
    uint64_t prev_end = hint;
    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= prev_end && v->start - prev_end >= len) return prev_end;
        if (v->end > prev_end) prev_end = v->end;
    }
    return prev_end;
}

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

    if (newv->next && newv->end == newv->next->start &&
        newv->vm_flags == newv->next->vm_flags &&
        newv->pte_flags == newv->next->pte_flags) {
        vm_area_t *nxt = newv->next;
        newv->end = nxt->end;
        newv->next = nxt->next;
        if (nxt->next) nxt->next->prev = newv;
        kfree(nxt);
    }
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

uint64_t prot_to_pte(int prot) {
    uint64_t f = PTE_V | PTE_U | PTE_A | PTE_D;
    if (prot & 1) f |= PTE_R;
    if (prot & 2) f |= PTE_W;
    if (prot & 4) f |= PTE_X;
    /* R=0,W=1 is a reserved encoding in Sv39 — force R when W is set. */
    if (f & PTE_W) f |= PTE_R;
    return f;
}

uint64_t mm_mmap(mm_struct_t *mm, uint64_t addr, size_t len, int prot, int flags) {
    len = ROUND_UP(len, PAGE_SIZE);
    if (len == 0) return (uint64_t)-EINVAL;

    uint64_t ptef = prot_to_pte(prot);
    uint64_t vmf = VM_ANON;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;

    if ((flags & MAP_FIXED) && addr != 0) {
        mm_munmap(mm, addr, len);
    } else if (addr != 0) {
        vm_area_t *existing = mm_find_vma(mm, addr);
        if (existing && existing->start < addr + len && existing->end > addr)
            addr = 0;
    }

    if (addr == 0)
        addr = mm_find_gap(mm, MMAP_BASE_ADDR, len);

    if (addr == 0) return (uint64_t)-ENOMEM;

    vm_area_t *vma = kcalloc(1, sizeof(vm_area_t));
    if (!vma) return (uint64_t)-ENOMEM;
    vma->start     = addr;
    vma->end       = addr + len;
    vma->vm_flags  = vmf;
    vma->pte_flags = ptef;

    mm_insert_vma(mm, vma);
    mm->total_vm += len / PAGE_SIZE;

    return addr;
}

int mm_munmap(mm_struct_t *mm, uint64_t addr, size_t len) {
    len = ROUND_UP(len, PAGE_SIZE);
    uint64_t end = addr + len;

    vm_area_t *vma = mm->mmap;
    while (vma) {
        vm_area_t *next = vma->next;
        if (vma->start >= end || vma->end <= addr) { vma = next; continue; }

        uint64_t clip_start = vma->start < addr ? addr : vma->start;
        uint64_t clip_end   = vma->end > end ? end : vma->end;

        for (uint64_t va = clip_start; va < clip_end; va += PAGE_SIZE) {
            paddr_t pa = pt_translate(mm->pgdir, va);
            pt_unmap(mm->pgdir, va);
            if (pa) { frame_put(phys_to_pfn(pa)); mm->rss--; }
        }
        mm->total_vm -= (clip_end - clip_start) / PAGE_SIZE;

        if (addr <= vma->start && end >= vma->end) {
            if (vma->prev) vma->prev->next = vma->next;
            else mm->mmap = vma->next;
            if (vma->next) vma->next->prev = vma->prev;
            kfree(vma);
        } else if (addr <= vma->start) {
            vma->start = clip_end;
        } else if (end >= vma->end) {
            vma->end = clip_start;
        } else {
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
    __asm__ volatile("sfence.vma" ::: "memory");
    return 0;
}

uint64_t mm_brk(mm_struct_t *mm, uint64_t newbrk) {
    if (newbrk == 0) return mm->brk;
    if (newbrk < mm->start_brk) return mm->brk;
    if (newbrk < mm->brk) {
        uint64_t new_brk_page = ROUND_UP(newbrk, PAGE_SIZE);
        uint64_t old_brk_page = ROUND_UP(mm->brk, PAGE_SIZE);
        for (uint64_t va = new_brk_page; va < old_brk_page; va += PAGE_SIZE) {
            uint64_t *pte = pt_walk(mm->pgdir, va, 0);
            if (pte && (*pte & PTE_V)) {
                paddr_t pa = SV39_PTE_ADDR(*pte);
                frame_put(phys_to_pfn(pa));
                mm->rss--;
                *pte = 0;
            }
        }
        if (new_brk_page < old_brk_page)
            __asm__ volatile("sfence.vma" ::: "memory");
    }
    mm->brk = newbrk;
    return mm->brk;
}

int mm_mprotect(mm_struct_t *mm, uint64_t addr, size_t len, int prot) {
    if (!mm || !mm->pgdir) return -EINVAL;
    len = ROUND_UP(len, PAGE_SIZE);
    uint64_t ptef = prot_to_pte(prot);
    uint64_t vmf = VM_ANON;
    if (prot & 1) vmf |= VM_READ;
    if (prot & 2) vmf |= VM_WRITE;
    if (prot & 4) vmf |= VM_EXEC;
    uint64_t end = addr + len;
    int touched = 0;

    for (vm_area_t *v = mm->mmap; v; v = v->next) {
        if (v->start >= end || v->end <= addr) continue;
        uint64_t s = v->start < addr ? addr : v->start;
        uint64_t e = v->end > end ? end : v->end;
        for (uint64_t va = s; va < e; va += PAGE_SIZE) {
            uint64_t *pte = pt_walk(mm->pgdir, va, 0);
            if (pte && *pte & PTE_V)
                *pte = (*pte & ~(PTE_R | PTE_W | PTE_X)) | (ptef & (PTE_R | PTE_W | PTE_X));
        }
        v->pte_flags = (v->pte_flags & ~(PTE_R | PTE_W | PTE_X)) | (ptef & (PTE_R | PTE_W | PTE_X));
        v->vm_flags  = (v->vm_flags & ~(VM_READ | VM_WRITE | VM_EXEC)) | vmf;
        touched = 1;
    }

    if (touched)
        __asm__ volatile("sfence.vma" ::: "memory");
    return 0;
}

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

    vm_area_t **tail = &child->mmap;
    vm_area_t *prev = NULL;
    for (vm_area_t *pv = parent->mmap; pv; pv = pv->next) {
        vm_area_t *cv = kcalloc(1, sizeof(vm_area_t));
        if (!cv) goto fail;
        *cv = *pv;
        cv->prev = prev;
        cv->next = NULL;
        *tail = cv;
        tail = &cv->next;
        prev = cv;
    }

    for (int vpn2 = 0; vpn2 < 256; vpn2++) {
        uint64_t pte2 = parent->pgdir[vpn2];
        if (!(pte2 & PTE_V)) continue;
        uint64_t *l1p = PTE_TO_PTR(pte2);
        int is_leaf1 = (pte2 & (PTE_R|PTE_W|PTE_X)) != 0;
        if (is_leaf1) continue;

        uint64_t *l1c = (uint64_t *)frame_alloc();
        if (!l1c) goto fail;
        memset(l1c, 0, PAGE_SIZE);
        child->pgdir[vpn2] = PTE_FROM_PA(va_to_pa(l1c)) | PTE_V;

        for (int vpn1 = 0; vpn1 < 512; vpn1++) {
            uint64_t pte1 = l1p[vpn1];
            if (!(pte1 & PTE_V)) continue;
            int is_leaf0 = (pte1 & (PTE_R|PTE_W|PTE_X)) != 0;
            if (is_leaf0) continue;

            uint64_t *l0p = PTE_TO_PTR(pte1);
            uint64_t *l0c = (uint64_t *)frame_alloc();
            if (!l0c) goto fail;
            memset(l0c, 0, PAGE_SIZE);
            l1c[vpn1] = PTE_FROM_PA(va_to_pa(l0c)) | PTE_V;

            for (int vpn0 = 0; vpn0 < 512; vpn0++) {
                uint64_t pte = l0p[vpn0];
                if (!(pte & PTE_V) || !(pte & PTE_U)) continue;

                pfn_t pfn = phys_to_pfn(SV39_PTE_ADDR(pte));
                frame_get(pfn);

                uint64_t new_flags = pte & (PTE_R | PTE_X | PTE_U | PTE_A | PTE_D | PTE_G);
                if (pte & PTE_W || pte & PTE_COW) {
                    new_flags &= ~(uint64_t)PTE_W;
                    new_flags |= PTE_COW;
                    l0p[vpn0] = (pte & ~PTE_W) | PTE_COW;
                }
                l0c[vpn0] = PTE_FROM_PA(SV39_PTE_ADDR(pte)) | new_flags | PTE_V;
            }
        }
    }

    __asm__ volatile("sfence.vma" ::: "memory");
    return child;
fail:
    mm_destroy(child);
    return NULL;
}

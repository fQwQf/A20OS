#include "mm/vm.h"

/*
 * PTE flag conversion helpers.
 *
 * These map between the Linux-style prot/vm_flags sets and the arch page-table
 * leaf flags.  They are pure bit arithmetic with no address-space state, so
 * they live in their own translation unit independent of the VMA machinery in
 * mm/vma.c and the mmap/munmap/mprotect/mremap paths in mm/vm.c.
 */

pte_t mm_prot_to_pte_flags(int prot) {
    pte_t f = PTE_V | PTE_U | PTE_A | PTE_MAT1 | PTE_LEAF;
    if (prot & 1) f |= PTE_R;
    if (prot & 2) f |= (PTE_W | PTE_D);
    if (prot & 4) f |= PTE_X;
    if (f & PTE_W) f |= PTE_R;
    return f;
}

int mm_pte_flags_to_prot(pte_t pte_flags) {
    int prot = 0;
    if (pte_flags & PTE_R) prot |= PROT_READ;
    if (pte_flags & PTE_W) prot |= PROT_WRITE;
    if (pte_flags & PTE_X) prot |= PROT_EXEC;
    return prot;
}

pte_t mm_vm_flags_to_pte_flags(uint64_t vm_flags) {
    int prot = 0;
    if (vm_flags & VM_READ) prot |= 1;
    if (vm_flags & VM_WRITE) prot |= 2;
    if (vm_flags & VM_EXEC) prot |= 4;
    return mm_prot_to_pte_flags(prot);
}

uint64_t mm_pte_flags_to_vm_flags(pte_t pte_flags) {
    uint64_t vm = 0;
    if (pte_flags & PTE_R) vm |= VM_READ;
    if (pte_flags & PTE_W) vm |= VM_WRITE;
    if (pte_flags & PTE_X) vm |= VM_EXEC;
    return vm;
}

pte_t mm_user_stack_pte_flags(void) {
    return mm_prot_to_pte_flags(PROT_READ | PROT_WRITE);
}

pte_t mm_user_brk_pte_flags(void) {
    return mm_prot_to_pte_flags(1 | 2);
}

int mm_pte_flags_allow_access(pte_t pte_flags) {
    return (pte_flags & (PTE_R | PTE_W | PTE_X)) != 0;
}

pte_t mm_pte_flags_apply_prot(pte_t old_flags, pte_t prot_flags) {
    pte_t flags = old_flags & (PTE_R | PTE_W | PTE_X | PTE_U |
                                  PTE_G | PTE_A | PTE_D | PTE_COW |
                                  PTE_LEAF | PTE_MAT1);
    flags &= ~(uint64_t)(PTE_R | PTE_W | PTE_X | PTE_D);
    flags |= prot_flags & (PTE_R | PTE_W | PTE_X | PTE_D);
    if (!(prot_flags & PTE_W))
        flags &= ~(uint64_t)PTE_COW;
    return flags;
}

pte_t mm_pte_flags_make_writable_dirty(pte_t pte_flags) {
    return pte_flags | PTE_W | PTE_D;
}

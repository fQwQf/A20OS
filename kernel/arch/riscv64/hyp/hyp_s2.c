/* RISC-V Sv39x4 stage-2 implementation. The upper three quarters of the
 * mandatory 16 KiB root remain zero until metadata supports 2048 entries. */
#include "hyp/hyp_vcpu.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "core/string.h"
#include "core/stdio.h"
#define HYP_GPA_LIMIT (1ULL << 39)
#if defined(ARCH_HAS_PGTABLE_OPS) && !defined(CONFIG_NOMMU)
int hyp_riscv_vm_init(hyp_vm_t *vm)
{
    pfn_t pfn = pfa_alloc(2);
    if (pfn == PFN_NONE)
        return -ENOMEM;
    pte_t *root = pfn_to_virt(pfn);
    memset(root, 0, PAGE_SIZE << 2);
    if (mm_pt_node_init(root, ARCH_PT_ROOT_LEVEL) != 0) {
        pfa_free(pfn, 2);
        return -ENOMEM;
    }
    vm->s2_root = root;
    vm->s2_root_order = 2;
    hyp_account_tables(4);
    return 0;
}
static void hyp_s2_destroy_level(pte_t *table, int level, unsigned order)
{
    if (!table)
        return;
    int entries = arch_pt_level_entries(level);
    for (int i = 0; i < entries; i++) {
        pte_t pte = table[i];
        if (!(pte & PTE_V))
            continue;
        paddr_t pa = arch_pte_addr(pte);
        pfn_t pfn = phys_to_pfn(pa);
        if (!pfn_valid(pfn)) {
            kerr("hyp: stage-2 corrupt entry level=%d idx=%d pte=%lx\n",
                 level, i, (unsigned long)pte);
            table[i] = 0;
            continue;
        }
        if (!arch_pte_is_leaf(pte)) {
            hyp_s2_destroy_level(arch_pte_to_ptr(pte), level - 1, 0);
        } else {
            mm_pt_frame_return(pfn);
            frame_put(pfn);
            hyp_account_mapped(-1);
        }
        table[i] = 0;
    }
    /* Same free shape as pt_free_table(): drop the metadata block, then the
     * frame. The backend root allocation can span more than one page. */
    mm_pt_node_fini(table);
    pfa_free(virt_to_pfn(table), order);
    hyp_account_tables(-(1 << order));
}

void hyp_riscv_vm_destroy(hyp_vm_t *vm)
{
    hyp_s2_destroy_level(vm->s2_root, ARCH_PT_ROOT_LEVEL, vm->s2_root_order);
}
static pte_t *hyp_s2_walk(hyp_vm_t *vm, uint64_t gpa, int alloc)
{
    if (gpa >= HYP_GPA_LIMIT)
        return NULL;
    pte_t *table = vm->s2_root;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 0; level--) {
        int idx = arch_pt_vpn(gpa, level);
        mm_pt_node_lock(table);
        pte_t pte = table[idx];
        mm_pt_node_unlock(table);
        if (!(pte & PTE_V)) {
            if (!alloc)
                return NULL;
            pte_t *next = frame_alloc_nr();
            if (!next)
                return NULL;
            memset(next, 0, PAGE_SIZE);
            if (mm_pt_node_init_atomic(next, level - 1) != 0) {
                pfa_free(virt_to_pfn(next), 0);
                return NULL;
            }
            mm_pt_node_lock(table);
            pte = table[idx];
            int installed = !(pte & PTE_V);
            if (installed) {
                pte = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
                table[idx] = pte;
                hyp_account_tables(1);
                mm_pt_note_present(table, level, idx, MM_ST_CLS_BYTE(MM_ST_PT_NODE));
            }
            mm_pt_node_unlock(table);
            if (!installed) {
                mm_pt_node_fini(next);
                pfa_free(virt_to_pfn(next), 0);
            }
        }
        if (arch_pte_is_leaf(pte))
            return NULL;
        table = arch_pte_to_ptr(pte);
    }
    return table;
}

static int hyp_s2_fence_all(hyp_vm_t *vm, uint64_t gpa)
{
    hyp_arch_s2_fence(vm->vmid, gpa);
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    uint32_t cpus = vm->active_cpu_mask;
    spin_unlock_irqrestore(&vm->control_lock, flags);
    return hyp_vm_remote_fence(vm, cpus, 2);
}

int hyp_riscv_s2_map(hyp_vm_t *vm, uint64_t gpa, pfn_t pfn, pte_t prot)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || gpa & (PAGE_SIZE - 1))
        return -EINVAL;
    if (gpa >= HYP_GPA_LIMIT)
        return -ERANGE;
    if (!pfn_valid(pfn))
        return -EINVAL;

    pte_t *table = hyp_s2_walk(vm, gpa, 1);
    if (!table)
        return -ENOMEM;
    int idx = arch_pt_vpn(gpa, 0);

    /* Stage-2 makes no privilege decision about the GUEST: the guest's own
     * stage-1 has already had its say by the time this table is walked.  It
     * is the G-stage WALK itself that runs at the privilege the guest's access
     * implies, and that privilege is U for a VS-mode access -- a leaf without
     * U is refused before its R/W/X are even looked at ("supervisor PTE flags
     * when not S mode", QEMU target/riscv/cpu_helper.c).  So U is set here
     * and a caller that wants a supervisor leaf cannot have one. */
    pte_t flags = (prot | PTE_R | PTE_A | PTE_D | PTE_U);

    mm_pt_node_lock(table);
    if (table[idx] & PTE_V) {
        mm_pt_node_unlock(table);
        return -EEXIST;
    }
    mm_pt_frame_lend(pfn);
    table[idx] = arch_pte_leaf(pfn_to_phys(pfn), flags);
    mm_pt_note_present(table, 0, idx,
                       (uint8_t)(MM_ST_CLS_BYTE(MM_ST_GUEST_MEM) |
                                 mm_pt_prot_bits(flags)));
    hyp_account_mapped(1);
    int rc = hyp_s2_fence_all(vm, gpa);
    mm_pt_node_unlock(table);
    /* Publication transferred ownership. On a failed remote fence retain
     * this backing, quarantine the VM and stop all vCPUs; never return it
     * as an unmapped caller-owned frame while a stale remote walk may exist. */
    if (rc) hyp_vm_fail(vm);
    return 0;
}

/* The caller owns the returned frame reference even if a concurrent loader
 * rollback unmaps the GPA. Holding a VM reference keeps table nodes alive. */
int hyp_riscv_s2_pin(hyp_vm_t *vm, uint64_t gpa, pfn_t *out)
{
    pte_t *table = hyp_s2_walk(vm, gpa, 0);
    if (!table) return -EFAULT;
    mm_pt_node_lock(table);
    pte_t pte = table[arch_pt_vpn(gpa, 0)];
    int rc = -EFAULT;
    if ((pte & PTE_V) && arch_pte_is_leaf(pte)) {
        pfn_t pfn = phys_to_pfn(arch_pte_addr(pte));
        if (pfn_valid(pfn)) { frame_get(pfn); *out = pfn; rc = 0; }
    }
    mm_pt_node_unlock(table);
    return rc;
}

int hyp_riscv_s2_unmap(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || gpa & (PAGE_SIZE - 1))
        return -EINVAL;

    pte_t *table = hyp_s2_walk(vm, gpa, 0);
    if (!table)
        return -ENOENT;
    int idx = arch_pt_vpn(gpa, 0);

    mm_pt_node_lock(table);
    pte_t pte = table[idx];
    if (!(pte & PTE_V) || !arch_pte_is_leaf(pte)) {
        mm_pt_node_unlock(table);
        return -ENOENT;
    }
    table[idx] = 0;
    mm_pt_note_absent(table, 0, idx);
    int rc = hyp_s2_fence_all(vm, gpa);
    if (rc) {
        table[idx] = pte;
        mm_pt_note_present(table, 0, idx,
                          MM_ST_CLS_BYTE(MM_ST_GUEST_MEM) | mm_pt_prot_bits(pte));
        mm_pt_node_unlock(table);
        hyp_vm_fail(vm);
        return rc;
    }
    mm_pt_node_unlock(table);
    pfn_t pfn = phys_to_pfn(arch_pte_addr(pte));
    mm_pt_frame_return(pfn);
    hyp_account_mapped(-1);
    return 0;
}

paddr_t hyp_riscv_s2_translate(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return 0;
    pte_t *table = hyp_s2_walk(vm, gpa, 0);
    if (!table)
        return 0;
    pte_t pte = table[arch_pt_vpn(gpa, 0)];
    if (!(pte & PTE_V) || !arch_pte_is_leaf(pte))
        return 0;
    return arch_pte_addr(pte);
}

int hyp_riscv_s2_audit(hyp_vm_t *vm, mm_pt_audit_report_t *out)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    return mm_s2_audit(vm->s2_root, ARCH_PT_ROOT_LEVEL, out);
}

#endif

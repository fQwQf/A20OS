/* AArch64 stage-2 and x86 nested paging share a radix shape, not a PTE ABI.
 * Keep pt_meta/frame ownership, but audit using the selected hardware format.
 * One VM lock covers software walks and activation; table pages live until
 * VM teardown. No reclaiming allocation is permitted under this lock. */
#include "hyp/hyp_s2.h"
#include "mm/frame.h"
#include "core/cpu.h"
#include "core/string.h"

#if (defined(ARCH_HAS_PGTABLE_OPS) || defined(ARCH_HAS_HYP_S2)) && !defined(CONFIG_NOMMU)
_Static_assert(PAGE_SIZE == 4096 && MM_PT_META_ENTRIES >= 512, "native stage-2 table shape");

static uint64_t *new_table(unsigned level)
{
    pfn_t pfn = pfa_alloc_flags(0, 0);
    if (pfn == PFN_NONE) return NULL;
    uint64_t *table = pfn_to_virt(pfn);
    memset(table, 0, PAGE_SIZE);
    if (mm_pt_node_init_atomic((pte_t *)table, level)) {
        pfa_free(pfn, 0);
        return NULL;
    }
    hyp_account_tables(1);
    return table;
}

int hyp_s2_tree_init(hyp_vm_t *vm, const hyp_s2_format_t *f)
{
    if (!vm || !vm->backend || !f || f->levels < 2 || f->levels > 4 ||
        !f->leaf_flags || !f->invalidate || !f->valid_mask || !f->address_mask ||
        (f->table_flags & f->address_mask) ||
        (f->table_flags & f->valid_mask) != f->valid_mask ||
        (f->address_mask & (PAGE_SIZE - 1)) ||
        vm->backend->gpa_limit > (1ULL << (12 + 9 * f->levels)))
        return -EINVAL;
    uint64_t *root = new_table(f->levels - 1);
    if (!root) return -ENOMEM;
    if (pfn_to_phys(virt_to_pfn(root)) & ~f->address_mask) {
        mm_pt_node_fini((pte_t *)root);
        pfa_free(virt_to_pfn(root), 0);
        hyp_account_tables(-1);
        return -ERANGE;
    }
    vm->s2_format = f;
    vm->s2_root = (pte_t *)root;
    vm->s2_root_order = 0;
    return 0;
}

static uint64_t *child_table(const hyp_s2_format_t *f, uint64_t entry)
{
    pfn_t pfn = phys_to_pfn(entry & f->address_mask);
    if (!pfn_valid(pfn) || !(pfa.meta[pfn].flags & FRAME_F_PT)) return NULL;
    return pfn_to_virt(pfn);
}

/* Caller holds s2_lock. Intermediate table encodings never have block leaves. */
static uint64_t *walk(hyp_vm_t *vm, uint64_t gpa, int alloc)
{
    const hyp_s2_format_t *f = vm->s2_format;
    if (!f || gpa >= vm->backend->gpa_limit) return NULL;
    uint64_t *table = (uint64_t *)vm->s2_root;
    for (unsigned level = f->levels - 1; level; level--) {
        unsigned idx = (gpa >> (12 + 9 * level)) & 511;
        uint64_t e = __atomic_load_n(&table[idx], __ATOMIC_ACQUIRE);
        if (!(e & f->valid_mask)) {
            if (!alloc) return NULL;
            uint64_t *next = new_table(level - 1);
            if (!next) return NULL;
            paddr_t pa = pfn_to_phys(virt_to_pfn(next));
            if (pa & ~f->address_mask) {
                mm_pt_node_fini((pte_t *)next);
                pfa_free(virt_to_pfn(next), 0);
                hyp_account_tables(-1);
                return NULL;
            }
            __atomic_store_n(&table[idx], pa | f->table_flags, __ATOMIC_RELEASE);
            mm_pt_note_present((pte_t *)table, level, idx, MM_ST_CLS_BYTE(MM_ST_PT_NODE));
            e = __atomic_load_n(&table[idx], __ATOMIC_ACQUIRE);
        }
        table = child_table(f, e);
        if (!table) return NULL;
    }
    return table;
}

static int remote_busy(hyp_vm_t *vm)
{
    uint64_t flags = spin_lock_irqsave(&vm->control_lock);
    uint32_t remote = vm->active_cpu_mask & ~(1u << cpu_current_id());
    spin_unlock_irqrestore(&vm->control_lock, flags);
    return remote ? -EBUSY : 0;
}

int hyp_s2_tree_map(hyp_vm_t *vm, uint64_t gpa, pfn_t pfn, pte_t prot)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || !vm->s2_format || (gpa & 4095)) return -EINVAL;
    if (gpa >= vm->backend->gpa_limit) return -ERANGE;
    if (!pfn_valid(pfn)) return -EINVAL;
    const hyp_s2_format_t *f = vm->s2_format;
    paddr_t pa = pfn_to_phys(pfn);
    if (pa & ~f->address_mask) return -ERANGE;
    uint64_t bits = f->leaf_flags(prot);
    if ((bits & f->valid_mask) != f->valid_mask || (bits & f->address_mask)) return -EINVAL;
    uint64_t irq = spin_lock_irqsave(&vm->s2_lock);
    int rc = vm->s2_format->live_update ? 0 : remote_busy(vm);
    if (rc) goto out;
    uint64_t *table = walk(vm, gpa, 1);
    if (!table) { rc = -ENOMEM; goto out; }
    unsigned idx = (gpa >> 12) & 511;
    if (__atomic_load_n(&table[idx], __ATOMIC_ACQUIRE) & f->valid_mask) { rc = -EEXIST; goto out; }
    /* Guest ownership is exclusive: a boolean FRAME_F_GUEST cannot represent
     * aliases. Take the flag atomically with validation across different VMs. */
    uint64_t pf = spin_lock_irqsave(&pfa.lock);
    if (!pfa.meta[pfn].refcount || (pfa.meta[pfn].flags & (FRAME_F_PT | FRAME_F_GUEST))) {
        spin_unlock_irqrestore(&pfa.lock, pf);
        rc = -EBUSY;
        goto out;
    }
    pfa.meta[pfn].flags |= FRAME_F_GUEST;
    spin_unlock_irqrestore(&pfa.lock, pf);
    __atomic_store_n(&table[idx], pa | bits, __ATOMIC_RELEASE);
    /* Host mm_pt_prot_bits deliberately reports no permissions without U.
     * Stage-2 has no host user/supervisor distinction. */
    uint8_t access = ((prot & PTE_R) ? MM_ST_PROT_R : 0) |
                     ((prot & PTE_W) ? MM_ST_PROT_W : 0) |
                     ((prot & HYP_PROT_EXEC) ? MM_ST_PROT_X : 0);
    mm_pt_note_present((pte_t *)table, 0, idx, MM_ST_CLS_BYTE(MM_ST_GUEST_MEM) | access);
    hyp_account_mapped(1);
    rc = f->invalidate(vm, gpa);
    /* Published pages stay owned even if invalidation fails. The caller must
     * not free a page a stale hardware walker could still reference. */
    if (rc) __atomic_store_n(&vm->broken, 1, __ATOMIC_RELEASE);
    rc = 0;
out:
    spin_unlock_irqrestore(&vm->s2_lock, irq);
    return rc;
}

int hyp_s2_tree_pin(hyp_vm_t *vm, uint64_t gpa, pfn_t *out)
{
    if (!vm || !out || !vm->s2_format) return -EINVAL;
    uint64_t irq = spin_lock_irqsave(&vm->s2_lock);
    uint64_t *table = walk(vm, gpa, 0);
    int rc = -EFAULT;
    if (table) {
        uint64_t e = __atomic_load_n(&table[(gpa >> 12) & 511], __ATOMIC_ACQUIRE);
        pfn_t pfn = phys_to_pfn(e & vm->s2_format->address_mask);
        if ((e & vm->s2_format->valid_mask) && pfn_valid(pfn)) {
            frame_get(pfn); *out = pfn; rc = 0;
        }
    }
    spin_unlock_irqrestore(&vm->s2_lock, irq);
    return rc;
}

int hyp_s2_tree_unmap(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || vm->magic != HYP_VM_MAGIC || !vm->s2_format || (gpa & 4095)) return -EINVAL;
    uint64_t irq = spin_lock_irqsave(&vm->s2_lock);
    int rc = remote_busy(vm);
    if (rc) goto out;
    uint64_t *table = walk(vm, gpa, 0);
    unsigned idx = (gpa >> 12) & 511;
    const hyp_s2_format_t *f = vm->s2_format;
    if (!table || !(__atomic_load_n(&table[idx], __ATOMIC_ACQUIRE) & f->valid_mask)) { rc = -ENOENT; goto out; }
    uint64_t e = __atomic_load_n(&table[idx], __ATOMIC_ACQUIRE);
    __atomic_store_n(&table[idx], 0, __ATOMIC_RELEASE);
    rc = f->invalidate(vm, gpa);
    if (rc) {
        __atomic_store_n(&table[idx], e, __ATOMIC_RELEASE);
        __atomic_store_n(&vm->broken, 1, __ATOMIC_RELEASE);
        goto out;
    }
    mm_pt_note_absent((pte_t *)table, 0, idx);
    mm_pt_frame_return(phys_to_pfn(e & f->address_mask));
    hyp_account_mapped(-1);
out:
    spin_unlock_irqrestore(&vm->s2_lock, irq);
    return rc;
}

paddr_t hyp_s2_tree_translate(hyp_vm_t *vm, uint64_t gpa)
{
    if (!vm || !vm->s2_format) return 0;
    uint64_t irq = spin_lock_irqsave(&vm->s2_lock);
    uint64_t *table = walk(vm, gpa, 0);
    uint64_t e = table ? __atomic_load_n(&table[(gpa >> 12) & 511], __ATOMIC_ACQUIRE) : 0;
    paddr_t pa = (e & vm->s2_format->valid_mask) ? e & vm->s2_format->address_mask : 0;
    spin_unlock_irqrestore(&vm->s2_lock, irq);
    return pa;
}

static void destroy(hyp_vm_t *vm, uint64_t *table, unsigned level)
{
    const hyp_s2_format_t *f = vm->s2_format;
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = table[i];
        if (!(e & f->valid_mask)) continue;
        if (level) {
            uint64_t *child = child_table(f, e);
            if (child) destroy(vm, child, level - 1);
        } else {
            pfn_t pfn = phys_to_pfn(e & f->address_mask);
            mm_pt_frame_return(pfn);
            frame_put(pfn);
            hyp_account_mapped(-1);
        }
    }
    mm_pt_node_fini((pte_t *)table);
    pfa_free(virt_to_pfn(table), 0);
    hyp_account_tables(-1);
}

void hyp_s2_tree_destroy(hyp_vm_t *vm)
{
    /* Last VM ref implies no vCPUs, pins or concurrent table users. Hardware
     * state must already have been detached by backend put(). */
    if (vm->s2_root && vm->s2_format) destroy(vm, (uint64_t *)vm->s2_root, vm->s2_format->levels - 1);
    vm->s2_root = NULL;
    vm->s2_format = NULL;
}

static void audit(hyp_vm_t *vm, uint64_t *table, unsigned level, mm_pt_audit_report_t *r)
{
    const hyp_s2_format_t *f = vm->s2_format;
    pt_meta_t *m = mm_pt_meta((pte_t *)table);
    r->pt_pages++;
    if (!m) { r->missing_meta++; return; }
    for (unsigned i = 0; i < 512; i++) {
        uint64_t e = table[i];
        uint8_t st = m->cls[i];
        if (!(e & f->valid_mask)) {
            if (st) r->absent_mismatch++;
            continue;
        }
        r->entries++;
        if (level) {
            if (MM_ST_GET_CLASS(st) != MM_ST_PT_NODE) r->present_mismatch++;
            uint64_t *child = child_table(f, e);
            if (!child) r->seg_bad_slot++;
            else audit(vm, child, level - 1, r);
        } else {
            if (MM_ST_GET_CLASS(st) != MM_ST_GUEST_MEM) r->present_mismatch++;
            pte_t prot = ((st & MM_ST_PROT_R) ? PTE_R : 0) |
                         ((st & MM_ST_PROT_W) ? PTE_W : 0) |
                         ((st & MM_ST_PROT_X) ? HYP_PROT_EXEC : 0);
            if ((e ^ f->leaf_flags(prot)) & f->leaf_check_mask) r->prot_mismatch++;
            pfn_t pfn = phys_to_pfn(e & f->address_mask);
            if (!pfn_valid(pfn) || !frame_is_lent_to_guest(pfn)) r->seg_extent_mismatch++;
        }
    }
}

int hyp_s2_tree_audit(hyp_vm_t *vm, mm_pt_audit_report_t *out)
{
    if (!vm || !vm->s2_format || !out) return -EINVAL;
    memset(out, 0, sizeof(*out));
    uint64_t irq = spin_lock_irqsave(&vm->s2_lock);
    audit(vm, (uint64_t *)vm->s2_root, vm->s2_format->levels - 1, out);
    spin_unlock_irqrestore(&vm->s2_lock, irq);
    return mm_pt_audit_errors(out) ? -EFAULT : 0;
}
#else
int hyp_s2_tree_init(hyp_vm_t *v, const hyp_s2_format_t *f) { (void)v; (void)f; return -EOPNOTSUPP; }
void hyp_s2_tree_destroy(hyp_vm_t *v) { (void)v; }
int hyp_s2_tree_map(hyp_vm_t *v, uint64_t g, pfn_t p, pte_t f) { (void)v; (void)g; (void)p; (void)f; return -EOPNOTSUPP; }
int hyp_s2_tree_pin(hyp_vm_t *v, uint64_t g, pfn_t *p) { (void)v; (void)g; (void)p; return -EOPNOTSUPP; }
int hyp_s2_tree_unmap(hyp_vm_t *v, uint64_t g) { (void)v; (void)g; return -EOPNOTSUPP; }
paddr_t hyp_s2_tree_translate(hyp_vm_t *v, uint64_t g) { (void)v; (void)g; return 0; }
int hyp_s2_tree_audit(hyp_vm_t *v, mm_pt_audit_report_t *o) { (void)v; (void)o; return -EOPNOTSUPP; }
#endif

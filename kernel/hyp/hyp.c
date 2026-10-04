/*
 * Hypervisor foundation, machine-independent half: the stage-2 address
 * space and the VM container.  See kernel/include/hyp/hyp.h for the
 * contract and docs/hypervisor/00-design.md for the design record.
 */
#include "hyp/hyp.h"
#include "mm/pt.h"
#include "mm/frame.h"
#include "mm/mm.h"
#include "mm/slab.h"
#include "core/panic.h"
#include "core/stdio.h"
#include "core/string.h"

/* Root entries this implementation programs.  Sv39x4 could address 4x this
 * through the same root shape; see the header comment before raising it. */
#define HYP_GPA_LIMIT (1ULL << 39)

/* One VMID per VM, never reused while the kernel runs: hgatp.VMID tags the
 * guest TLB, and handing a dead VM's tag to a new VM would let stale guest
 * translations survive an hfence that names the tag.  A counter costs one
 * fence per VM creation instead.  Width: hgatp.VMID is implementation
 * defined (QEMU: 14 bits); the counter wraps long after any real run, and
 * the wrap is a documented limit, not a silent alias. */
static uint16_t hyp_next_vmid;

hyp_vm_t *hyp_vm_create(uint64_t mem_size)
{
    hyp_vm_t *vm = kcalloc(1, sizeof(*vm));
    if (!vm)
        return NULL;
    pte_t *root = (pte_t *)frame_alloc();
    if (!root) {
        kfree(vm);
        return NULL;
    }
    memset(root, 0, PAGE_SIZE);
    if (mm_pt_node_init(root, ARCH_PT_ROOT_LEVEL) != 0) {
        pfa_free(virt_to_pfn(root), 0);
        kfree(vm);
        return NULL;
    }
    vm->magic    = HYP_VM_MAGIC;
    vm->s2_root  = root;
    vm->vmid     = hyp_next_vmid++;
    vm->mem_size = mem_size;
    vm->refcount = 1;
    /* Fresh VMID: no guest translation under this tag can exist anywhere. */
    hyp_arch_vmid_fenced(vm->vmid);
    return vm;
}

/* Destroy one stage-2 tree.  Callers guarantee no vcpu of this VM is
 * running and no host thread touches the tree (one reference, held by the
 * destroyer), so a direct recursive free is safe -- the same contract
 * pt_destroy() relies on for a dying address space.  Unlike pt_destroy,
 * leaves here are OURS: stage-2 leaves name frames lent to the VM, so the
 * walk returns each one. */
static void hyp_s2_destroy_level(pte_t *table, int level)
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
            hyp_s2_destroy_level(arch_pte_to_ptr(pte), level - 1);
        } else {
            mm_pt_frame_return(pfn);
            frame_put(pfn);
        }
        table[i] = 0;
    }
    /* Same free shape as pt_free_table(): drop the metadata block, then the
     * frame.  The root was allocated at order 0 like any table here. */
    mm_pt_node_fini(table);
    pfa_free(virt_to_pfn(table), 0);
}

void hyp_vm_put(hyp_vm_t *vm)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return;
    if (--vm->refcount > 0)
        return;
    hyp_s2_destroy_level(vm->s2_root, ARCH_PT_ROOT_LEVEL);
    vm->magic = 0;
    kfree(vm);
}

/*
 * Stage-2 walk.  The shape mirrors the host pt_map_cls()/pt_unmap_leaf()
 * pair, with two differences the header records: leaves carry
 * MM_ST_GUEST_MEM, and installing one lends the frame.
 *
 * Intermediate nodes are allocated with the NON-reclaiming allocator:
 * hyp_s2_map holds the parent node's lock while installing a child, and a
 * reclaiming allocation under that lock reaches oom_try_reclaim(), whose
 * victim teardown wants node locks -- the exact hazard that pinned
 * frame_alloc_nr() for the host cursor (docs 11.7).
 */
static pte_t *hyp_s2_walk(hyp_vm_t *vm, uint64_t gpa, int alloc)
{
    pte_t *table = vm->s2_root;
    for (int level = ARCH_PT_ROOT_LEVEL; level > 0; level--) {
        int idx = arch_pt_vpn(gpa, level);
        pte_t pte = table[idx];
        if (pte & PTE_V) {
            if (arch_pte_is_leaf(pte))
                return NULL;    /* a huge leaf covers the whole slot */
            table = arch_pte_to_ptr(pte);
            continue;
        }
        if (!alloc)
            return NULL;
        pte_t *next = (pte_t *)frame_alloc_nr();
        if (!next)
            return NULL;
        memset(next, 0, PAGE_SIZE);
        mm_pt_node_init(next, level - 1);
        table[idx] = arch_pte_from_pa(va_to_pa(next)) | PTE_DIR;
        mm_pt_note_present(table, level, idx,
                           MM_ST_CLS_BYTE(MM_ST_PT_NODE));
        table = next;
    }
    return table;
}

int hyp_s2_map(hyp_vm_t *vm, uint64_t gpa, pfn_t pfn, pte_t prot)
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
    table[idx] = arch_pte_leaf(pfn_to_phys(pfn), flags);
    mm_pt_note_present(table, 0, idx,
                       (uint8_t)(MM_ST_CLS_BYTE(MM_ST_GUEST_MEM) |
                                 mm_pt_prot_bits(flags)));
    mm_pt_node_unlock(table);

    /* The frame becomes the guest's: flag it (identity side; the reference
     * the caller holds is what keeps it alive), then fence the stale
     * translation for this VMID/page -- a remap after unmap must not read
     * the old entry out of a guest TLB. */
    mm_pt_frame_lend(pfn);
    hyp_arch_s2_fence(vm->vmid, gpa);
    return 0;
}

int hyp_s2_unmap(hyp_vm_t *vm, uint64_t gpa)
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
    mm_pt_node_unlock(table);

    pfn_t pfn = phys_to_pfn(arch_pte_addr(pte));
    mm_pt_frame_return(pfn);
    /* The reference goes back to the caller, which owns it again. */
    hyp_arch_s2_fence(vm->vmid, gpa);
    return 0;
}

paddr_t hyp_s2_translate(hyp_vm_t *vm, uint64_t gpa)
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

int hyp_s2_audit(hyp_vm_t *vm, mm_pt_audit_report_t *out)
{
    if (!vm || vm->magic != HYP_VM_MAGIC)
        return -EINVAL;
    return mm_s2_audit(vm->s2_root, ARCH_PT_ROOT_LEVEL, out);
}

/* ---- kernel-side selftest (docs/hypervisor/00-design.md, S4) ---- */

static int st_fail(const char *step)
{
    kinfo("HYP_SELFTEST: FAIL at %s\n", step);
    return -1;
}

int hyp_selftest(void)
{
    if (!hyp_supported()) {
        kinfo("HYP_SELFTEST: SKIP (no virtualization extension)\n");
        return 0;
    }

    kinfo("HYP_SELFTEST: create\n");
    hyp_vm_t *vm = hyp_vm_create(4 << 20);
    if (!vm)
        return st_fail("create");

    /* Map four pages at the demo guest's RAM base, each holding its own
     * pattern, and read them back THROUGH the stage-2 translation -- that
     * is the pair (gpa->host pa) and (host pa -> bytes) agreeing. */
    static const uint64_t base = 0x80000000ULL;
    pfn_t pfns[4];
    for (int i = 0; i < 4; i++) {
        pfns[i] = pfa_alloc_page();
        if (pfns[i] == PFN_NONE)
            return st_fail("alloc");
        memset(pfn_to_virt(pfns[i]), 0x41 + i, PAGE_SIZE);
        if (hyp_s2_map(vm, base + (uint64_t)i * PAGE_SIZE, pfns[i],
                       PTE_R | PTE_W) != 0)
            return st_fail("map");
    }
    for (int i = 0; i < 4; i++) {
        paddr_t pa = hyp_s2_translate(vm, base + (uint64_t)i * PAGE_SIZE);
        if (pa != pfn_to_phys(pfns[i]))
            return st_fail("translate");
        if (*(volatile uint8_t *)pfn_to_virt(pfns[i]) != (uint8_t)(0x41 + i))
            return st_fail("readback");
    }

    /* The lend is visible on both sides: every mapped frame carries
     * FRAME_F_GUEST, and the stage-2 audit cross-checks flag against
     * mapping -- a lent-but-unmapped or mapped-but-unlent frame fails
     * here. */
    for (int i = 0; i < 4; i++)
        if (!frame_is_lent_to_guest(pfns[i]))
            return st_fail("lend-flag");
    mm_pt_audit_report_t rep;
    if (hyp_s2_audit(vm, &rep) != 0)
        return st_fail("audit");
    if (rep.entries == 0 || rep.seg_bad_slot != 0)
        return st_fail("audit-counters");

    /* Unmap one page: translation goes away and the frame comes back
     * (flag cleared, still holding the caller's reference). */
    if (hyp_s2_unmap(vm, base + 2 * PAGE_SIZE) != 0)
        return st_fail("unmap");
    if (hyp_s2_translate(vm, base + 2 * PAGE_SIZE) != 0)
        return st_fail("unmap-translate");
    if (frame_is_lent_to_guest(pfns[2]))
        return st_fail("unmap-return");
    if (hyp_s2_map(vm, base + 2 * PAGE_SIZE, pfns[2], PTE_R | PTE_W) != 0)
        return st_fail("remap");
    if (hyp_s2_map(vm, base + 2 * PAGE_SIZE, pfns[2], PTE_R | PTE_W) != -EEXIST)
        return st_fail("remap-eexist");

    /* Destroy: every lent frame returns with its reference dropped, so the
     * buddy sees exactly the four frames this test allocated, free again. */
    hyp_vm_put(vm);
    for (int i = 0; i < 4; i++) {
        if (frame_is_lent_to_guest(pfns[i]))
            return st_fail("destroy-flag");
        frame_put(pfns[i]);
    }

    kinfo("HYP_SELFTEST: PASS (vmid=%u entries=%lu)\n",
          0, (unsigned long)rep.entries);
    return 0;
}

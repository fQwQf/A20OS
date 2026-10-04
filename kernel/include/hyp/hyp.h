#ifndef _HYP_H
#define _HYP_H

#include "core/types.h"
#include "mm/pt.h"
#include "hyp/hyp_arch.h"

/*
 * Hypervisor foundation: guest physical address spaces (stage-2) and the VM
 * container around them.  Design record: docs/hypervisor/00-design.md.
 *
 * The load-bearing decision is that a stage-2 tree is a page table of the
 * SAME shape as a host one and reuses the single-level model's machinery
 * wholesale:
 *
 *   - every stage-2 page-table page carries a pt_meta_t (same node MCS lock,
 *     same per-entry status byte), so the concurrency and audit story of
 *     docs/roadmap/single-level-mm-model.md applies unchanged;
 *   - a mapped stage-2 leaf carries class MM_ST_GUEST_MEM -- never a host
 *     anon/file class, which would describe the wrong plane -- and the host
 *     auditor counts GUEST_MEM in a host table as a mismatch;
 *   - the frame a leaf names carries FRAME_F_GUEST for as long as the
 *     mapping exists (mm_pt_frame_lend/return), and mm_s2_audit()
 *     cross-checks flag against mapping so a frame whose VM died without
 *     returning it cannot hide;
 *   - hgatp is Sv39x4, but this file only ever programs root entries
 *     [0,512): guest physical memory must live below 512 GiB (HYP_GPA_LIMIT).
 *     The demo guest lives at 0x80000000; lifting the limit means an x4 root
 *     (2048 entries, 16 KiB) and a wider root metadata block, which is a
 *     format change to pt_meta_t and is deliberately deferred.
 *
 * Everything here runs in host kernel context on behalf of a host process.
 * The vcpu run loop (register save/restore, guest trap entry) is arch code;
 * see kernel/arch/riscv64/hyp/.
 */

typedef struct hyp_vm {
    uint32_t magic;
#define HYP_VM_MAGIC 0x48594d56 /* 'HYMV' */
    pte_t    *s2_root;      /* stage-2 root; pt_meta_t attached (level 2) */
    uint16_t  vmid;         /* hgatp.VMID; TLB tag for this address space */
    uint64_t  mem_size;     /* guest RAM the VM is provisioned for */
    int       refcount;
} hyp_vm_t;

/* Feature probe: 1 when the CPU implements the virtualization extension and
 * the arch layer can drive it.  Cheap enough to call per syscall. */
int  hyp_supported(void);

/* Create a VM with an empty stage-2.  mem_size is bookkeeping for now (the
 * guest RAM is provisioned page by page through hyp_s2_map); it caps nothing
 * yet and that is documented, not hidden.  Returns NULL on failure. */
hyp_vm_t *hyp_vm_create(uint64_t mem_size);
void      hyp_vm_put(hyp_vm_t *vm);

/* Map one 4 KiB guest page: gpa -> host frame, lending the frame to the VM.
 * prot is a host PTE flag set; PTE_U is stripped (stage-2 has no U bit).
 * Returns 0, or a negative errno.  -EEXIST when the gpa is already mapped. */
int  hyp_s2_map(hyp_vm_t *vm, uint64_t gpa, pfn_t pfn, pte_t prot);
/* Remove one 4 KiB guest page mapping, returning the frame. */
int  hyp_s2_unmap(hyp_vm_t *vm, uint64_t gpa);
/* Translate gpa -> host physical address, or 0 when unmapped.  Audit/test
 * helper; the guest hardware does its own walks. */
paddr_t hyp_s2_translate(hyp_vm_t *vm, uint64_t gpa);

/* Audit the stage-2 tree against its metadata (mm_s2_audit) plus the
 * frame-flag cross-check.  Returns 0 when clean. */
int  hyp_s2_audit(hyp_vm_t *vm, mm_pt_audit_report_t *out);

/* Kernel-side selftest, run by reading /proc/a20/hyp_selftest.  Creates a
 * VM, maps and verifies pages, audits, unmaps, destroys, and writes one
 * HYP_SELFTEST line per step.  Returns 0 when every step passed. */
int  hyp_selftest(void);

#endif /* _HYP_H */

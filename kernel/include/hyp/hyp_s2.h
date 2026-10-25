#ifndef A20_HYP_S2_H
#define A20_HYP_S2_H
#include "hyp/hyp.h"

/* Native 4 KiB, 512-entry radix tables. Hardware descriptor encodings belong
 * to the backend, never to the host's stage-1 PTE helpers. Format lifetime
 * must cover the VM. Only bottom-level leaves are supported. */
typedef struct hyp_s2_format {
    unsigned levels;
    uint64_t address_mask, valid_mask, table_flags;
    uint64_t (*leaf_flags)(pte_t prot);
    /* Called with IRQs disabled and s2_lock held, after publication. Must
     * synchronously retire local translations before backing can be reused.
     * Every backend must also flush on load (including CPU migration). */
    int (*invalidate)(hyp_vm_t *vm, uint64_t gpa);
    /* Zero rejects updates while a remote CPU has this VM claimed. A backend
     * enabling this must implement synchronous remote invalidation as well. */
    int live_update;
    uint64_t leaf_check_mask; /* audit excludes hardware-maintained A/D bits */
} hyp_s2_format_t;

int hyp_s2_tree_init(hyp_vm_t *vm, const hyp_s2_format_t *format);
void hyp_s2_tree_destroy(hyp_vm_t *vm);
int hyp_s2_tree_map(hyp_vm_t *vm, uint64_t gpa, pfn_t pfn, pte_t prot);
int hyp_s2_tree_pin(hyp_vm_t *vm, uint64_t gpa, pfn_t *pfn);
int hyp_s2_tree_unmap(hyp_vm_t *vm, uint64_t gpa);
paddr_t hyp_s2_tree_translate(hyp_vm_t *vm, uint64_t gpa);
int hyp_s2_tree_audit(hyp_vm_t *vm, mm_pt_audit_report_t *out);
#endif

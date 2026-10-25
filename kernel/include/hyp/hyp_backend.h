#ifndef A20_HYP_BACKEND_H
#define A20_HYP_BACKEND_H
#include "core/types.h"
#include "mm/pt.h"
/* Short-descriptor host MMUs may expose no semantic executable bit. Guest
 * stage-2 permissions still need one; keep it out of the host PTE ABI. */
#define HYP_PROT_EXEC ((pte_t)(PTE_X ? PTE_X : (1UL << 31)))
struct hyp_vm;
struct hyp_vcpu;
/* Internal interface shared by all ABI adapters. Capabilities describe the
 * implemented backend, never merely a CPU feature. ABI numbers live elsewhere. */
typedef struct hyp_backend {
    const char *name;
    unsigned s2_root_order;
    unsigned max_running_vcpus;
    uint64_t gpa_limit;
    uint64_t ram_base;
    int (*probe_smp)(struct hyp_vm *vm);
    int (*remote_fence)(struct hyp_vm *vm, uint32_t cpus, unsigned kind);
    void (*reset)(struct hyp_vcpu *vcpu, uint64_t entry, uint64_t hart, uint64_t opaque);
    int (*vm_init)(struct hyp_vm *vm);
    void (*vm_destroy)(struct hyp_vm *vm);
    int (*map)(struct hyp_vm *vm, uint64_t gpa, pfn_t pfn, pte_t prot);
    int (*pin)(struct hyp_vm *vm, uint64_t gpa, pfn_t *pfn);
    int (*unmap)(struct hyp_vm *vm, uint64_t gpa);
    paddr_t (*translate)(struct hyp_vm *vm, uint64_t gpa);
    int (*audit)(struct hyp_vm *vm, mm_pt_audit_report_t *out);
    int (*set_boot)(struct hyp_vcpu *vcpu, uint64_t hartid, uint64_t dtb);
    void (*sync_irqs)(struct hyp_vcpu *vcpu, unsigned set, unsigned clear);
    int (*load)(struct hyp_vcpu *vcpu);
    void (*enter)(struct hyp_vcpu *vcpu);
    void (*put)(struct hyp_vcpu *vcpu);
    /* Release lazily allocated architecture state after the last vCPU ref. */
    void (*vcpu_destroy)(struct hyp_vcpu *vcpu);
} hyp_backend_t;
const hyp_backend_t *hyp_arch_backend(void);
#endif

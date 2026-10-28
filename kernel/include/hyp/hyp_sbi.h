#ifndef A20_HYP_SBI_H
#define A20_HYP_SBI_H
#include "hyp/hyp_vcpu.h"

/* Guest-visible SBI identity. 0xa20 is locally chosen, NOT an allocated ID. */
#define HYP_SBI_SPEC_VERSION 2u /* 0.2; newer optional extensions are probed */
#define HYP_SBI_IMPL_ID 0xa20u
#define HYP_SBI_IMPL_VERSION 2u
#define HYP_SBI_BASE 0x10u
#define HYP_SBI_TIME 0x54494d45u
#define HYP_SBI_HSM 0x48534du
#define HYP_SBI_IPI 0x735049u
#define HYP_SBI_RFENCE 0x52464e43u
#define HYP_SBI_SRST 0x53525354u
#define HYP_SBI_DBCN 0x4442434eu
/* Bounded shared-buffer validation in interrupt-disabled trap context. Larger
 * buffers are outside this implementation's shared-memory contract. */
#define HYP_SBI_CONSOLE_MAX_BUFFER 65536u
#define HYP_SBI_CONSOLE_QUANTUM 256u

/* Only VS supervisor ecalls enter this dispatcher. Returns 1 to resume, 0
 * for a resumable host yield, hart stop or VM shutdown. */
int hyp_riscv_sbi_ecall(hyp_vcpu_t *vcpu, uint64_t scause);
pte_t hyp_riscv_s2_prot(hyp_vm_t *vm, uint64_t gpa);
int hyp_riscv_s2_pin_access(hyp_vm_t *vm, uint64_t gpa, pte_t need, pfn_t *out);
void hyp_riscv_local_fence(unsigned kind);
#endif

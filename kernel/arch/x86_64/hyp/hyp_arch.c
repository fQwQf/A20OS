/* Select a complete hardware backend; feature bits alone do not suffice. */
#include "hyp/hyp_vcpu.h"
int hyp_svm_supported(void);
int hyp_vmx_supported(void);
const hyp_backend_t *hyp_svm_backend(void);
const hyp_backend_t *hyp_vmx_backend(void);
int hyp_supported(void) { return hyp_svm_supported() || hyp_vmx_supported(); }
const hyp_backend_t *hyp_arch_backend(void)
{ return hyp_svm_supported() ? hyp_svm_backend() : hyp_vmx_backend(); }
int hyp_arch_vcpu_setup(hyp_vcpu_t *v) { return hyp_arch_backend()->load(v); }
void hyp_arch_vcpu_enter(hyp_vcpu_t *v) { hyp_arch_backend()->enter(v); }
/* Legacy RISC-V hooks are never used by the SVM run loop. */
void hyp_arch_vmid_fenced(uint16_t vmid) { (void)vmid; }
void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa) { (void)vmid; (void)gpa; }
uint64_t hyp_arch_vsatp_get(void) { return 0; }
void hyp_arch_vsatp_set(uint64_t v) { (void)v; }
uint64_t hyp_arch_vstimecmp_get(void) { return 0; }
void hyp_arch_vstimecmp_set(uint64_t v) { (void)v; }
void hyp_arch_host_tlb_fence(void) { }

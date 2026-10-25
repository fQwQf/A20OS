/*
 * RV32 can implement H, but needs Sv32x4 tables and XLEN=32 guest/host
 * register banking. The RV64 Sv39x4 backend and its 64-bit assembly cannot
 * be reused by selecting an H-capable CPU.
 * Keep both availability interfaces closed until an operational backend
 * implements stage-2 isolation, guest entry and complete host restoration.
 */
#include "hyp/hyp_arch.h"
#include "hyp/hyp_backend.h"
#include "hyp/hyp_vcpu.h"
#include "core/errno.h"
#include "core/klog.h"

int hyp_supported(void)
{
    return 0;
}

const hyp_backend_t *hyp_arch_backend(void)
{
    return NULL;
}

void hyp_arch_vmid_fenced(uint16_t vmid)
{
    (void)vmid;
}

void hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa)
{
    (void)vmid;
    (void)gpa;
}

/* Legacy RISC-V hooks are inert while this backend is unavailable. */
uint64_t hyp_arch_vsatp_get(void)
{
    return 0;
}

void hyp_arch_vsatp_set(uint64_t satp)
{
    (void)satp;
}

uint64_t hyp_arch_vstimecmp_get(void)
{
    return 0;
}

void hyp_arch_vstimecmp_set(uint64_t deadline)
{
    (void)deadline;
}

void hyp_arch_host_tlb_fence(void)
{
}

/* Refuse setup without mutating the caller's guest or host state. */
int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu)
{
    (void)vcpu;
    return -EOPNOTSUPP;
}

void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu)
{
    /* A direct call must not be mistaken for successful guest execution. */
    (void)vcpu;
    kerr("hyp: riscv32 guest backend unavailable\n");
}

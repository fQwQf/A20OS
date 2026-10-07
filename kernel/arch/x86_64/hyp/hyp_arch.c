/*
 * Arch stub: this CPU has no virtualization extension the kernel can drive,
 * so hyp_supported() answers 0 and the generic half's selftest reports SKIP.
 * The fences are never called when nothing can map a guest page, and the CSR
 * hooks below are never called either: hyp_vcpu_run() is the only path that
 * reaches them and it cannot get a vcpu without a VM, which cannot exist
 * without a stage-2 tree, which needs hyp_supported() to be 1.
 */
#include "hyp/hyp_arch.h"
#include "hyp/hyp_vcpu.h"
#include "core/errno.h"
#include "core/klog.h"

int hyp_supported(void)
{
    return 0;
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

/* The guest CSR hooks, answered in the only way an architecture with no such
 * CSR can: read zero, drop writes.  Read-zero is not an approximation here
 * either -- it is what the register would read on the one architecture that
 * has one, at MODE=Bare, before a guest has turned its own paging on. */
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

/* The vcpu half has no assembly here either: this architecture cannot enter a
 * guest at all.  setup() refuses with the contract's -ENOTSUP (this tree spells
 * it EOPNOTSUPP, exactly as the riscv64 half does), and enter() sits behind
 * that refusal.  They are defined rather than left undefined because
 * kernel/hyp/hyp_vcpu.c is compiled for EVERY architecture and calls both --
 * the generic half does not, and cannot, condition itself on an arch feature. */
int hyp_arch_vcpu_setup(hyp_vcpu_t *vcpu)
{
    (void)vcpu;
    return -EOPNOTSUPP;
}

void hyp_arch_vcpu_enter(hyp_vcpu_t *vcpu)
{
    /* Unreachable through the run loop: hyp_vcpu_run() calls setup() first and
     * returns on the refusal above.  Saying so loudly, rather than returning
     * quietly, keeps a caller that skipped setup() from believing a guest ran
     * -- the run loop would go on to report -EIO on an untouched vcpu. */
    (void)vcpu;
    kerr("hyp: guest entry on an architecture with no virtualization\n");
}

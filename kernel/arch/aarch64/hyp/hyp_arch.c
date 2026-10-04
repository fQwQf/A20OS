/*
 * Arch stub: this CPU has no virtualization extension the kernel can drive,
 * so hyp_supported() answers 0 and the generic half's selftest reports SKIP.
 * The fences are never called when nothing can map a guest page.
 */
#include "hyp/hyp_arch.h"

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

#ifndef _HYP_ARCH_H
#define _HYP_ARCH_H

/*
 * Arch half of the hypervisor foundation.  Each architecture that can host
 * guests provides these three; architectures without a virtualization
 * extension provide the stub versions (hyp_supported() == 0) so the generic
 * half and the selftest compile everywhere and report SKIP.
 */
#include "core/types.h"

int     hyp_supported(void);
void    hyp_arch_vmid_fenced(uint16_t vmid);
void    hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa);

#endif /* _HYP_ARCH_H */

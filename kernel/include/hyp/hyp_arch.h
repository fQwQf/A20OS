#ifndef _HYP_ARCH_H
#define _HYP_ARCH_H

/*
 * Arch half of the hypervisor foundation.  Each architecture that can host
 * guests provides these; architectures without a virtualization extension
 * provide the stub versions (hyp_supported() == 0) so the generic half and the
 * selftest compile everywhere and report SKIP.
 *
 * Everything here is HOST state -- the CSR file and the TLB invalidation the
 * host itself performs.  Naming them is what keeps the generic half
 * (kernel/hyp/) free of arch mnemonics: a run loop that needs the guest's
 * vsatp asks for it here instead of writing `csrr 0x280` into a file that also
 * has to assemble for architectures that have no such CSR.  The guest run loop
 * in particular decodes guest CSR accesses, and the CSR NUMBERS it decodes
 * (0x180, 0x14d) are a RISC-V encoding fact that stays in that file as data;
 * only the ACT of touching the register belongs to this header.
 */
#include "core/types.h"

int     hyp_supported(void);
void    hyp_arch_vmid_fenced(uint16_t vmid);
void    hyp_arch_s2_fence(uint16_t vmid, uint64_t gpa);

/* The guest's own stage-1 root, as the host sees it.  Read by the generic
 * half to walk a guest page table in software (the host MMU walks the host's
 * satp, never this one); written by it when it emulates the guest's satp write,
 * because with hstatus.VTVM that write traps to HS-mode and has to be
 * performed here for the guest to have a table at all.
 *
 * The stub reads 0 -- MODE=Bare, i.e. "the guest translates nothing", which is
 * also the reset value of the register on the one architecture that has it --
 * and drops writes.  It is never reached on a stub: hyp_supported() is 0, so
 * no vcpu is ever created, let alone run. */
uint64_t hyp_arch_vsatp_get(void);
void     hyp_arch_vsatp_set(uint64_t satp);

/* The guest's Sstc comparator, read and written on its behalf the same way:
 * the legacy SBI set_timer call and an emulated guest vstimecmp write both go
 * through here.  Stub: reads 0, drops writes. */
uint64_t hyp_arch_vstimecmp_get(void);
void     hyp_arch_vstimecmp_set(uint64_t deadline);

/* Invalidate the translations THIS hart may reuse on the host side, i.e. the
 * host-execution-context fence, not the stage-2 one above (that is
 * hyp_arch_s2_fence and it is per-VMID).  The run loop issues it for a guest
 * SFENCE.VMA executed under hstatus.VTVM: the guest asked to drop its own
 * mappings, and on a CPU where the instruction is ungated in HS-mode the way
 * to honour that is to drop everything this hart has cached.  Stub: no-op. */
void     hyp_arch_host_tlb_fence(void);

#endif /* _HYP_ARCH_H */

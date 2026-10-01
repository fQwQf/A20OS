#ifndef _ARCH_LOONGARCH64_H
#define _ARCH_LOONGARCH64_H

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

/* User-space rdtime.d is available at PLV3; the kernel maps the vDSO/vvar
 * pages into every Linux-ABI task so clock_gettime/gettimeofday avoid a
 * full trap round trip (kernel/vdso/loongarch64/vdso.S). */
#define ARCH_HAS_VDSO 1

/* Linux exposes CPUCFG.1.UAL as HWCAP_LOONGARCH_UAL.  QEMU's LoongArch
 * TCG backend requires this contract because generated host loads/stores may
 * be naturally unaligned.  Do not advertise it on implementations that do
 * not report the architectural capability. */
static inline uintptr_t loongarch64_elf_hwcap(void)
{
    uint32_t cpucfg1;

    __asm__ __volatile__("cpucfg %0, %1" : "=r"(cpucfg1) : "r"(1U));
    return (cpucfg1 & (1U << 20)) ? (1UL << 2) : 0;
}

#define ARCH_ELF_HWCAP() loongarch64_elf_hwcap()


/* This architecture can dump its own CSRs at panic and at the first switch
 * back to user; see kernel/arch/loongarch64/platform/debug.c.  Gated on the
 * same flag that compiles that file, so a build without bring-up tracing gets
 * the no-op inline from core/arch.h rather than a declared-but-undefined
 * function. */
#ifdef CONFIG_DEBUG_BOOT_TRACE
#define ARCH_HAS_BOOT_TRACE_DUMPS 1
#endif

#endif

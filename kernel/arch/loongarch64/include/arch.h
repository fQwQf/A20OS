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

#endif

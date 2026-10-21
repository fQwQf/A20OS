#ifndef _ARCH_AARCH64_H
#define _ARCH_AARCH64_H

#define ARCH_NAME "aarch64"

#define ARCH_HAS_VDSO 1

#include "platform.h"
#include "console.h"
#include "cpu.h"
#include "page_table.h"
#include "trap_frame.h"
#include "firmware.h"

/* AArch64 is the only port that describes its translation state around a COW
 * leaf rewrite, so the opt-in diagnostic (mm/fault.c) is enabled from here
 * rather than by an architecture test in generic code. */
#ifdef CONFIG_MM_COW_TLBI_DIAG
# define ARCH_COW_TLBI_DIAG 1
#endif
/* Print the raw faulting VA as the TLBI operand instead of the shifted
 * address field, to compare the two encodings. */
#ifdef CONFIG_AARCH64_TLBI_DIAG_RAW_OPERAND
# define ARCH_COW_TLBI_DIAG_RAW_OPERAND 1
#endif

#endif

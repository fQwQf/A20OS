#ifndef _SBI_H
#define _SBI_H

/*
 * Legacy SBI compatibility shim.
 *
 * Architecture-specific firmware calls are now in arch/firmware.h
 * (included via arch.h → defs.h). This header is kept for backward
 * compatibility with existing #include "sbi.h" references.
 *
 * All sbi_* functions are declared in arch/firmware.h.
 */

#include "arch.h"

#endif

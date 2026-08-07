#ifndef _CORE_KALLSYMS_H
#define _CORE_KALLSYMS_H

#include "core/types.h"

/*
 * Kernel symbol table for oops backtrace symbolication.
 *
 * The generated table (kernel/core/kallsyms.c, produced by
 * tools/gen_kallsyms.py during the build's second link pass) contains the
 * .text-range symbols of the final kernel image, sorted by address.
 */

#ifndef KALLSYMS_COUNT
#define KALLSYMS_COUNT 0
#endif

/* Return the nearest symbol at or below `addr` and its byte offset, or NULL
 * if the address has no known symbol. */
const char *kallsyms_lookup(uint64_t addr, uint64_t *offset_out);

/* Print a symbolized address ("name+0xN" when known, "0x... " otherwise). */
void kallsyms_print(uint64_t addr);

uint32_t kallsyms_count(void);

#endif /* _CORE_KALLSYMS_H */

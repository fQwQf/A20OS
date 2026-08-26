#ifndef _CORE_BOOTARGS_H
#define _CORE_BOOTARGS_H

#include "core/types.h"

/* Arch-specific weak hook: returns a pointer to the null-terminated kernel
 * command line, or NULL if no command line is available.  The RISC-V and
 * aarch64 platforms override this with a DTB parser that extracts
 * /chosen/bootargs. */
const char *arch_bootargs_get(void);

/* Extract /chosen/bootargs from a Flattened Device Tree.  dtb_va must be
 * directly dereferenceable in the caller's address space; returns 0 on
 * success, -1 when absent or malformed. */
int fdt_extract_bootargs(const void *dtb_va, char *out, size_t outsz);

/* Return the kernel command line extracted at boot.  May return NULL or an
 * empty string if no command line was supplied. */
const char *bootargs_get(void);

/* Early init: parse and cache the command line.  Safe to call before mm_init
 * on architectures that do not need dynamic allocation. */
void bootargs_init(void);

#endif /* _CORE_BOOTARGS_H */

#ifndef _CORE_BOOTARGS_H
#define _CORE_BOOTARGS_H

#include "core/types.h"

/* Arch-specific weak hook: returns a pointer to the null-terminated kernel
 * command line, or NULL if no command line is available.  The RISC-V and
 * aarch64 platforms override this with a DTB parser that extracts
 * /chosen/bootargs. */
const char *arch_bootargs_get(void);

#if defined(CONFIG_UART_CMDLINE)
/* Prompt for a command line on the console and copy it into @out.
 *
 * Only compiled when CONFIG_UART_CMDLINE=y, which the build does not set by
 * default: this moves authority over kernel configuration -- including the
 * a20.xlator.<guest> path -- from whoever built the image to whoever holds
 * the serial port at boot.  It cannot be turned on from the command line,
 * because reading the command line is what it is for.
 *
 * Returns the length written, or -1 when nothing usable was entered (empty
 * line, or the timeout expiring).  The caller is expected to treat -1 as
 * "boot without one", which is the behaviour an image had before this
 * existed.  Call it from an arch_bootargs_get() fallback, not from
 * bootargs_init(), so an architecture with a working firmware command line
 * never prompts.
 */
int uart_cmdline_read(char *out, size_t outsz);
#endif

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

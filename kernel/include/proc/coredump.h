#ifndef _PROC_COREDUMP_H
#define _PROC_COREDUMP_H

/*
 * ELF core dump generation for fatal signals (kernel/proc/coredump.c).
 *
 * The fatal-signal default-action path in signal.c calls
 * coredump_on_fatal_signal() through a weak-symbol hook just before
 * proc_exit_group(); the strong definition lives in coredump.c and emits an
 * ET_CORE file per the /proc/sys/kernel/core_pattern setting.  The weak
 * default in signal.c is a no-op so MCU/profile builds that do not link
 * coredump.c keep working.
 *
 * Boundaries of the basic implementation:
 * - 64-bit architectures only (riscv64 primary; the gregset layout mapping is
 *   exact for riscv64, best-effort generic for the others).
 * - core_pattern "|pipe" mode is not supported (the dump is skipped and a
 *   klog note is emitted).
 * - Placeholder subset: %p %e %s %u %g %t %%; other % sequences are dropped.
 * - NT_FILE notes and core-limit-aware hole compression are not emitted;
 *   non-present pages are written as zero-filled.
 */

#include "core/types.h"
#include "core/trap.h"

/* Called by the fatal-signal path with the crashing task as proc_current().
 * Emits the core file when the signal's default action dumps core, the
 * process has a user mm, RLIMIT_CORE permits it, and core_pattern is a file
 * pattern. */
/* Returns 1 when a core file was actually written, 0 when the dump was
 * suppressed (no mm, RLIMIT_CORE==0, pipe pattern, allocation/write failure).
 * The caller uses that to decide whether to set the 0x80 WCOREDUMP bit. */
int coredump_on_fatal_signal(int sig, trap_context_t *ctx);

/* /proc/sys/kernel/core_pattern backing store.  set strips a trailing
 * newline; get appends one.  Both return 0 or a negative errno. */
int  coredump_set_pattern(const char *buf, size_t len);
int  coredump_get_pattern(char *buf, size_t bufsz);

#endif /* _PROC_COREDUMP_H */

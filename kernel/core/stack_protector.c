/*
 * A20OS — kernel stack canary runtime
 *
 * Supplies the two symbols -fstack-protector-strong expects from the compiler
 * ABI:
 *   - __stack_chk_guard: the global canary.  During the early boot window
 *     (before random_init()) it falls back to a compile-time constant so the
 *     early C code that runs from the very beginning is protected too; once the
 *     entropy pool is ready, stack_protector_init() replaces it with a random
 *     value whose low byte is cleared, mimicking a terminator canary so a
 *     string-style overflow cannot overwrite it in passing.
 *   - __stack_chk_fail: panics with the call site on a failed check.
 *
 * Timing constraint: when the guard is swapped there is exactly one frame on
 * the boot stack, kernel_main (each architecture's assembly jumps straight
 * into it), and kernel_main never returns, so the "old value pushed, new value
 * checked" false positive cannot occur.  The MCU profile does not link
 * core/random.c, so the canary keeps its fixed value there.
 */

#include "core/types.h"
#include "core/defs.h"
#include "core/panic.h"
#include "core/stack_protector.h"
#ifndef CONFIG_MCU
#include "core/random.h"
#endif

uintptr_t __stack_chk_guard = (uintptr_t)0xA20C0DEC0DE5AF00ULL;

void stack_protector_init(void)
{
#ifndef CONFIG_MCU
    __stack_chk_guard = (uintptr_t)(random_u64() & ~(uintptr_t)0xFF);
#endif
}

NORETURN void __stack_chk_fail(void)
{
    panic("kernel stack smashing detected (caller=%p)",
          __builtin_return_address(0));
}

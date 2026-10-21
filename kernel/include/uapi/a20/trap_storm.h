#ifndef _UAPI_A20_TRAP_STORM_H
#define _UAPI_A20_TRAP_STORM_H

/*
 * Fault-injection contract for the trap-storm detector's own test.
 *
 * Built with TRAP_STORM_TEST=1, the kernel reports a page fault at this
 * address as repaired without touching the mapping.  The faulting instruction
 * therefore re-executes and refaults at the same pc/va/cause with no signal
 * and no syscall in between -- the "repaired yet re-trapping" shape the
 * detector in kernel/core/trap.c exists to catch.  Normal builds treat the
 * address like any other unmapped one.
 *
 * The address is inside the user half of every supported port's address space
 * and below the interpreter/mmap/stack regions (mm/user_layout.h), so nothing
 * legitimate is ever mapped there.
 */
#define A20_TRAP_STORM_TEST_VA 0x20000000UL

#endif /* _UAPI_A20_TRAP_STORM_H */

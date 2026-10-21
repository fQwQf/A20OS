#ifndef _UAPI_A20_FUTEX_COW_WINDOW_H
#define _UAPI_A20_FUTEX_COW_WINDOW_H

/*
 * Fault-injection contract for the futex COW-window test.
 *
 * A private anonymous page at this address is the only place a
 * FUTEX_COW_WINDOW_TEST build perturbs.  On the first futex operation from a
 * task that touches it, the kernel skips the COW resolution that
 * user_prepare_write() would perform, leaving the leaf exactly in the
 * write-protected, frame-shared state a concurrent fork would have produced
 * between that resolution and the locked re-validation.  The re-validation
 * must then refuse, the operation must retry, and the retry -- with the
 * injection spent -- must complete on its own private frame.
 *
 * With the locked re-validation removed, the atomic store lands in the shared
 * frame and the forked child observes the parent's write, which is what the
 * test's negative control checks.
 *
 * The address sits in the user half of every supported port and below the
 * interpreter/mmap/stack regions (mm/user_layout.h), so nothing legitimate is
 * mapped there.  Normal builds ignore the address entirely.
 */
#define A20_FUTEX_COW_TEST_VA 0x22000000UL

#endif /* _UAPI_A20_FUTEX_COW_WINDOW_H */

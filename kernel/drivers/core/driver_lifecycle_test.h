/*
 * A20OS synthetic driver lifecycle test.
 *
 * Exercises register/match/probe/remove/re-probe/unregister paths. Guarded by
 * CONFIG_DRIVER_LIFECYCLE_TEST so normal bringup is unaffected.
 */
#ifndef _DRIVER_LIFECYCLE_TEST_H
#define _DRIVER_LIFECYCLE_TEST_H

#ifdef CONFIG_DRIVER_LIFECYCLE_TEST

/* Run the full synthetic lifecycle exercise once. Returns 0 on PASS, -1 on FAIL. */
int driver_lifecycle_test_run(void);

#else

static inline int driver_lifecycle_test_run(void) { return 0; }

#endif /* CONFIG_DRIVER_LIFECYCLE_TEST */

#endif /* _DRIVER_LIFECYCLE_TEST_H */

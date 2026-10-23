#ifndef A20_UAPI_TASK_STATUS_H
#define A20_UAPI_TASK_STATUS_H

/* exit_code remains a normal code, or 128+signum for a fatal signal.
 * Versioned task_exit may supply a notification cause; scalar exit is unchanged.
 * A core reason is reported only when the kernel status has its core bit set. */
#define A20_TASK_EXIT_NORMAL 0u
#define A20_TASK_EXIT_SIGNAL 1u
#define A20_TASK_EXIT_CORE 2u
#endif

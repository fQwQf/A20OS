/* Single-owner execution contract for the isolated FreeBSD protocol core. */
#ifndef A20_NET_BSD_EXECUTOR_H
#define A20_NET_BSD_EXECUTOR_H

#include "core/types.h"

typedef int (*a20_bsd_executor_job_t)(void *argument);
typedef void (*a20_bsd_executor_progress_t)(void *argument);

/* Start after proc_init(). The worker and the initialized BSD domain deliberately
 * have kernel lifetime; unloading a live protocol stack is not part of this API. */
int a20_bsd_executor_start(void);
int a20_bsd_executor_ready(void);
int a20_bsd_executor_is_current(void);

/* A process-context job may sleep until the owning worker completes it. Jobs
 * submitted by the worker itself execute directly and therefore must not wait
 * on another job. IRQ and timer paths only use notify, never call. */
int a20_bsd_executor_call(a20_bsd_executor_job_t job, void *argument);
void a20_bsd_executor_notify(void);

/* Runs in the owning worker after jobs and whenever NIC/timer/progress work
 * wakes it. Registration must precede start. */
void a20_bsd_executor_set_progress(a20_bsd_executor_progress_t progress,
                                   void *argument);

#endif

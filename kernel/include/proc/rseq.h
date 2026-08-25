#ifndef _PROC_RSEQ_H
#define _PROC_RSEQ_H

#include "proc/proc.h"

/* Publish the task's current CPU topology into its registered userspace
 * rseq area (cpu_id_start/cpu_id/node_id/mm_cid).  Writes go through the
 * kernel direct map and are skipped when the page is not resident; called
 * at registration and on every context switch dispatch. */
void rseq_publish(task_t *t);

#endif /* _PROC_RSEQ_H */

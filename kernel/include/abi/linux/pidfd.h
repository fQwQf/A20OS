#ifndef _ABI_LINUX_PIDFD_H
#define _ABI_LINUX_PIDFD_H

#include "proc/proc.h"

/* Return a referenced task pinned by this pidfd; pair success with proc_put(). */
task_t *linux_pidfd_get_task_ref(int pidfd);

#endif /* _ABI_LINUX_PIDFD_H */

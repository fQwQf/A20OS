#ifndef _ABI_LINUX_PIDFD_H
#define _ABI_LINUX_PIDFD_H

#include "proc/proc.h"
#include "fs/vfs.h"

/* Return a referenced task pinned by this pidfd; pair success with proc_put(). */
task_t *linux_pidfd_get_task_ref(int pidfd);

/* The pid a pidfd refers to, or -1 when @vf is not a pidfd (or its task is
 * gone).  /proc/<pid>/fdinfo/<fd> reports it as "Pid:", which dbus reads to
 * resolve its own credentials on Linux. */
int linux_pidfd_vfile_pid(const vfile_t *vf);

#endif /* _ABI_LINUX_PIDFD_H */

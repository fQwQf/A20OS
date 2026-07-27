#ifndef _IPC_SIGNALFD_H
#define _IPC_SIGNALFD_H

#include "fs/vfs.h"

/*
 * Linux signalfd(2) backend.  kernel_mask uses the internal encoding
 * (bit position == signum); conversion happens at the syscall boundary.
 */
int signalfd_create(int ufd, uint64_t kernel_mask, int flags);

/*
 * Returns poll revents (>= 0) if vf is a signalfd, or a negative value
 * when vf is not a signalfd.  Readiness is evaluated against the pending
 * signals of the calling task, matching signalfd read() semantics.
 */
int signalfd_poll_events(vfile_t *vf, short events);

#endif /* _IPC_SIGNALFD_H */

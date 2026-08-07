#ifndef _IPC_CHANNEL_FD_H
#define _IPC_CHANNEL_FD_H

#include "core/types.h"

struct a20_channel_ep;

/*
 * Install a channel endpoint as a file descriptor in the current task.
 * @ep must carry one reference which the fd owns (released on close());
 * the fd is read/write/poll-able with one-message-per-syscall semantics.
 */
int a20_channel_fd_install(struct a20_channel_ep *ep, int flags);

#endif /* _IPC_CHANNEL_FD_H */

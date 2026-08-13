#ifndef _FS_SPLICE_H
#define _FS_SPLICE_H

#include <stddef.h>

/*
 * Core splice(2)/tee(2)/vmsplice(2) engine (kernel/fs/splice.c).
 *
 * These operate on global fd numbers and kernel-side offset values; the Linux
 * ABI wrapper (kernel/abi/linux/sys_advise_copy.c) validates flags, resolves
 * user pointers, and enforces the pipe/no-offset rules before calling in.
 */

int splice_do(int fd_in, long *off_in, int fd_out, long *off_out, size_t len,
              int nonblock);
int tee_do(int fd_in, int fd_out, size_t len, int nonblock);
int vmsplice_do(int fd, const void *iov, int nr_segs, int nonblock);

#endif /* _FS_SPLICE_H */

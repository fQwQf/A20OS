#ifndef _FS_IO_URING_H
#define _FS_IO_URING_H

/*
 * io_uring subset for A20OS.
 *
 * Implements the classic io_uring submission/completion model on top of the
 * synchronous VFS: io_uring_setup() creates a ring (submission queue of
 * io_uring_sqe entries and completion queue of io_uring_cqe entries) stored
 * in kernel memory and returns a context id through an anonymous file.
 * io_uring_enter() drains the submission queue, executes the supported
 * opcodes synchronously through the VFS, and fills the completion queue.
 * io_uring_register() accepts the file-registration and eventfd options.
 *
 * The ring buffers live in kernel memory; A20OS does not mmap them into the
 * caller because userland (musl/liburing-less tools) drives everything
 * through io_uring_enter().
 */

#include "core/types.h"

#define IORING_MAX_ENTRIES 4096
#define IORING_MAX_RINGS   16

/* Opcodes supported by the synchronous executor. */
#define IORING_OP_NOP         0
#define IORING_OP_READV       1
#define IORING_OP_WRITEV      2
#define IORING_OP_FSYNC       3
#define IORING_OP_READ        5
#define IORING_OP_WRITE       6
#define IORING_OP_CLOSE       7

/* Registration types. */
#define IORING_REGISTER_FILES       2
#define IORING_REGISTER_EVENTFD     4

/* Create a ring with @entries capacity.  Returns a global VFS fd or a
 * negative errno. */
int io_uring_create(unsigned entries);

/* Submit/completion: process up to @to_submit submissions already copied
 * into the ring's submission queue and copy up to @min_complete completions
 * out.  Returns the number of submissions processed, or a negative errno. */
long io_uring_enter(int gfd, unsigned to_submit, unsigned min_complete,
                    unsigned flags);

/* Register files/eventfd.  Returns 0 or a negative errno. */
int io_uring_register(int gfd, unsigned opcode, const void *arg,
                      unsigned nr_args);

#endif /* _FS_IO_URING_H */

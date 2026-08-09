#ifndef _IPC_POSIX_MQ_H
#define _IPC_POSIX_MQ_H

/*
 * POSIX message queues (mq_open/mq_unlink/mq_getsetattr/mq_notify/
 * mq_timedsend/mq_timedreceive), ABI-independent.
 *
 * Each message queue is a named kernel object with a descriptor table per
 * process (the mqd is an integer descriptor, like an fd but independent of
 * the VFS fd table).  Blocking send/recv use the park/wake protocol.
 */

#include "core/types.h"

struct task_t;

#define MQ_NAME_MAX 256

/* mq_attr wire layout (Linux: 4 x long, 32 bytes on 64-bit). */
typedef struct mq_attr_kern {
    int64_t mq_flags;
    int64_t mq_maxmsg;
    int64_t mq_msgsize;
    int64_t mq_curmsgs;
} mq_attr_kern_t;

/* mq_open(2): create or open a queue by name.  Returns a descriptor (>=0)
 * or a negative errno. */
int posix_mq_open(const char *name, int oflag, int mode,
                  const mq_attr_kern_t *attr);

/* mq_unlink(2). */
int posix_mq_unlink(const char *name);

/* mq_getsetattr(2): if @newattr is non-NULL it is applied; @oldattr (kernel
 * buffer) receives the previous attributes. */
int posix_mq_getsetattr(int mqd, const mq_attr_kern_t *newattr,
                        mq_attr_kern_t *oldattr);

/* mq_notify(2): register a signal notification.  The sigevent is passed as a
 * kernel copy; a NULL sigevent unregisters. */
int posix_mq_notify(int mqd, const void *sigevent);

/* mq_timedsend(2): @msg is a kernel buffer of @msg_len bytes, @prio 0..31.
 * @deadline is an absolute tick deadline (0 = block forever). */
int posix_mq_timedsend(int mqd, const char *msg, size_t msg_len,
                       unsigned prio, uint64_t deadline);

/* mq_timedreceive(2): receives up to @msg_len bytes into @msg (kernel buffer),
 * stores the priority into @prio if non-NULL.  Returns payload length or a
 * negative errno. */
long posix_mq_timedreceive(int mqd, char *msg, size_t msg_len,
                           unsigned *prio, uint64_t deadline);

/* Close a descriptor (from close(2) on the mq fd). */
int posix_mq_close(int mqd);

/* Drop all descriptors owned by a task (task teardown). */
void posix_mq_release_task(struct task_t *t);

#endif /* _IPC_POSIX_MQ_H */

#ifndef _IPC_SYSV_MSG_H
#define _IPC_SYSV_MSG_H

/*
 * SysV message queues (msgget/msgsnd/msgrcv/msgctl), ABI-independent.
 *
 * A20OS keeps a fixed-size message queue table guarded by a single global
 * lock, following the pattern of kernel/ipc/sysv_sem.c.  Blocking send/recv
 * use the park/wake protocol through per-queue wait queues.
 */

#include "core/types.h"

struct task_t;

/* msgget(2).  Returns the queue id or a negative errno. */
int sysv_msg_get(int key, int msgflg);

/* msgsnd(2): @msgp points to a user msg layout, @msgsz is the payload size.
 * The ABI layer resolves the user pointer; this entry point receives a kernel
 * copy of {long mtype; char mtext[msgsz]}. */
int sysv_msg_send(int msqid, const void *msgp, size_t msgsz, int msgflg);

/* msgrcv(2): copies up to @msgsz bytes of payload into the kernel buffer at
 * @msgp (which is sized for the full message).  Returns the number of payload
 * bytes copied, or a negative errno. */
long sysv_msg_recv(int msqid, void *msgp, size_t msgsz, int64_t msgtyp,
                   int msgflg);

/* msgctl(2).  @arg is a kernel buffer for IPC_STAT/IPC_SET, NULL otherwise. */
int sysv_msg_control(int msqid, int cmd, void *arg);

#endif /* _IPC_SYSV_MSG_H */

#ifndef _MM_PROCESS_VM_H
#define _MM_PROCESS_VM_H

/*
 * Cross-process memory transfer helpers (process_vm_readv/writev).
 *
 * These are ABI-independent entry points that copy between the current
 * task's address space and another task's address space using the target
 * task's page table directly.  They are the building block for the Linux
 * process_vm_readv(2)/process_vm_writev(2) syscalls and also for a future
 * Native ABI cross-process VMO copy.
 */

#include "core/types.h"

struct task_t;

/* Copy @len bytes from @src_task's user address @src to the current task's
 * kernel buffer @dst.  Returns the number copied or a negative errno. */
long process_vm_read_kernel(struct task_t *src_task, const void *src,
                            void *dst, size_t len);

/* Copy @len bytes from the current task's kernel buffer @src to @dst_task's
 * user address @dst.  Returns the number copied or a negative errno. */
long process_vm_write_kernel(struct task_t *dst_task, void *dst,
                             const void *src, size_t len);

#endif /* _MM_PROCESS_VM_H */

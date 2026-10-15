#ifndef _MM_SHUTDOWN_WRITE_H
#define _MM_SHUTDOWN_WRITE_H

#include "core/types.h"

/*
 * Shared-file writable-mapping shutdown transaction.
 *
 * begin closes admission and returns a generation token without waiting.
 * The caller must wait in a schedulable context until drained returns 1,
 * then revoke (which synchronously shoots down translations and harvests
 * dirty cache pages).  On a pre-teardown failure call abort before reopening
 * the VFS namespace gate.  Once teardown begins, commit is terminal.
 */
int mm_shared_write_shutdown_begin(uint64_t *token);
int mm_shared_write_shutdown_drained(uint64_t token);
int mm_shared_write_shutdown_revoke(uint64_t token);
int mm_shared_write_shutdown_abort(uint64_t token);
int mm_shared_write_shutdown_commit(uint64_t token);

/* Admission count for a shared-file write or writable-PTE grant. Callers may
 * be preempted and some operations may allocate while counted, but must not
 * wait for filesystem I/O, shutdown completion, or any resource whose release
 * depends on shutdown. Never wait for admission while holding an MM, PTE,
 * page-cache, or VFS lock. */
int  mm_shared_write_enter(void);
/* Wait outside MM/page-table locks when shutdown has temporarily closed the
 * gate. Returns -EIO after a terminal commit. */
int  mm_shared_write_enter_wait(void);
void mm_shared_write_leave(void);
int  mm_shared_write_is_committed(void);
int  mm_shared_write_is_closed(void);

#endif

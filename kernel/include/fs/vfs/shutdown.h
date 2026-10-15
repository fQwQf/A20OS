#ifndef _FS_VFS_SHUTDOWN_H
#define _FS_VFS_SHUTDOWN_H

#include "core/types.h"
#include <stddef.h>

typedef int (*vfs_shutdown_phase_fn)(void *context);

typedef struct vfs_shutdown_ops {
    void *context;
    vfs_shutdown_phase_fn begin;
    vfs_shutdown_phase_fn sync;
    vfs_shutdown_phase_fn teardown;
    vfs_shutdown_phase_fn abort;
    vfs_shutdown_phase_fn commit;
} vfs_shutdown_ops_t;

_Static_assert(__builtin_offsetof(vfs_shutdown_ops_t, context) == 0,
               "shutdown callback ABI context offset");
#if __SIZEOF_POINTER__ == 8
_Static_assert(__builtin_offsetof(vfs_shutdown_ops_t, begin) == 8 &&
               __builtin_offsetof(vfs_shutdown_ops_t, sync) == 16 &&
               __builtin_offsetof(vfs_shutdown_ops_t, teardown) == 24 &&
               __builtin_offsetof(vfs_shutdown_ops_t, abort) == 32 &&
               __builtin_offsetof(vfs_shutdown_ops_t, commit) == 40 &&
               sizeof(vfs_shutdown_ops_t) == 48,
               "LP64 shutdown callback ABI");
#elif __SIZEOF_POINTER__ == 4
_Static_assert(__builtin_offsetof(vfs_shutdown_ops_t, begin) == 4 &&
               __builtin_offsetof(vfs_shutdown_ops_t, sync) == 8 &&
               __builtin_offsetof(vfs_shutdown_ops_t, teardown) == 12 &&
               __builtin_offsetof(vfs_shutdown_ops_t, abort) == 16 &&
               __builtin_offsetof(vfs_shutdown_ops_t, commit) == 20 &&
               sizeof(vfs_shutdown_ops_t) == 24,
               "ILP32 shutdown callback ABI");
#else
#error Unsupported shutdown callback pointer width
#endif

int a20_vfs_shutdown_run(const vfs_shutdown_ops_t *ops);

#endif

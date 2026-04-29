#ifndef _FS_LOCKS_H
#define _FS_LOCKS_H

#include "fs/vfs.h"

#define FS_LOCK_OWNER_PID 1
#define FS_LOCK_OWNER_OFD 2

typedef struct fs_flock {
    short l_type;
    short l_whence;
    int64_t l_start;
    int64_t l_len;
    int l_pid;
} fs_flock_t;

int fs_locks_get(vfile_t *vf, fs_flock_t *lk, int owner_kind, int owner);
int fs_locks_set(vfile_t *vf, const fs_flock_t *lk, int owner_kind, int owner, int wait);
int fs_flocks_apply(vfile_t *vf, int operation);
void fs_locks_release_process(int pid);
void fs_locks_release_file(vfile_t *vf, int gfd);

#endif /* _FS_LOCKS_H */

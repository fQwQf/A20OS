#ifndef _FS_PIPE_H
#define _FS_PIPE_H

#include "fs/vfs.h"

int pipe_vfile_is(vfile_t *vf);
int pipe_create(int pipefd[2]);
/* Registration is nonblocking under the VFS mutation gate. Rendezvous runs
 * after the gate has been released, with an owned file reference. */
vfile_t *fifo_open_vnode(vnode_t *vn, int flags);
int fifo_finish_open(vfile_t *vf);
/* Return owned endpoint references without allocating POSIX descriptors. */
int pipe_create_vfiles(vfile_t **read_end, vfile_t **write_end, int flags);
int pipe_poll_events(vfile_t *vf, short events);
int pipe_get_available(vfile_t *vf);
int pipe_get_size(vfile_t *vf);
int pipe_set_size(vfile_t *vf, size_t size);
int pipe_peek(vfile_t *vf, char *buf, size_t count);

#endif /* _FS_PIPE_H */

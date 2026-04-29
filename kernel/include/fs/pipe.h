#ifndef _FS_PIPE_H
#define _FS_PIPE_H

#include "fs/vfs.h"

int pipe_vfile_is(vfile_t *vf);
int pipe_create(int pipefd[2]);
int pipe_poll_events(vfile_t *vf, short events);
int pipe_get_size(vfile_t *vf);
int pipe_set_size(vfile_t *vf, size_t size);

#endif /* _FS_PIPE_H */

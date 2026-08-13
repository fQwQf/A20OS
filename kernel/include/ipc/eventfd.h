#ifndef _IPC_EVENTFD_H
#define _IPC_EVENTFD_H

#include "core/types.h"

struct vfile;

int eventfd_create(unsigned initval, int flags);
int eventfd_vfile_is(struct vfile *vf);

#endif /* _IPC_EVENTFD_H */

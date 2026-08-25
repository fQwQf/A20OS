#ifndef _FS_MEMFD_H
#define _FS_MEMFD_H

#include <stddef.h>

struct task_t;
struct vfile;

int memfd_create_file(int flags);
int memfd_set_contents(int fd, const void *data, size_t len);

/* memfd_secret(2): anonymous fd whose mappings and fd-theft are restricted
 * to the creating euid. */
int memfd_secret_file(int flags);
int vfile_is_memfd_secret(struct vfile *vf);
int memfd_secret_may_access(struct vfile *vf, struct task_t *caller);

#endif /* _FS_MEMFD_H */

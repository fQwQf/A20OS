#ifndef _FS_MEMFD_H
#define _FS_MEMFD_H

#include <stddef.h>

int memfd_create_file(int flags);
int memfd_set_contents(int fd, const void *data, size_t len);

#endif /* _FS_MEMFD_H */

#ifndef _FS_XATTR_H
#define _FS_XATTR_H

#include "core/types.h"
#include "fs/vfs.h"

#define XATTR_NAME_MAX_LOCAL 64
#define XATTR_VALUE_MAX_LOCAL 512
#define XATTR_CREATE 1
#define XATTR_REPLACE 2

int64_t xattr_set_vnode(vnode_t *vn, const char *name,
                        const void *value, size_t size, int flags);
int64_t xattr_get_vnode(vnode_t *vn, const char *name,
                        void *value, size_t size);
int64_t xattr_list_vnode(vnode_t *vn, char *list, size_t size);
int64_t xattr_remove_vnode(vnode_t *vn, const char *name);

#endif /* _FS_XATTR_H */

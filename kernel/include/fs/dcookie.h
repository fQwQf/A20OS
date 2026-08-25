#ifndef _FS_DCOOKIE_H
#define _FS_DCOOKIE_H

#include "core/types.h"

struct vnode;
typedef struct vnode vnode_t;

/* dcookie registry backing lookup_dcookie(2).  Kernel producers (profiling,
 * tracing) obtain an opaque cookie for an open path and hand it to
 * userspace; SYS_lookup_dcookie resolves it back to that path. */

uint64_t dcookie_register(vnode_t *vn, const char *path);
void dcookie_revoke(uint64_t cookie);

/* Resolve @cookie to its registered path.  Returns a table index valid
 * until dcookie_release(idx), or -1 when unknown. */
int dcookie_resolve(uint64_t cookie, char **path_out);
void dcookie_release(int idx);

#endif /* _FS_DCOOKIE_H */

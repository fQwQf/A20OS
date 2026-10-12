#ifndef A20_FLAT_FILES_H
#define A20_FLAT_FILES_H
#include "fs/fat32lite.h"
#include "fs/vfs.h"
/* Optional small Native file profile. Bind once before application publication;
 * callbacks and their context must remain valid for the kernel lifetime.
 * The read-only filesystem is shared, serialized, and independent of ISA/board.
 * This does not publish the full VFS/descriptor or HANDLES capability. */
int a20_flat_files_bind(const fat32lite_io_t *io, uint32_t partition_lba);
/* Snapshot an immutable file without changing its shared open offset. Caller
 * frees the returned allocation. Bound before allocating to cap RAM use. */
int a20_flat_file_image(vfile_t *file, size_t limit, void **data,
                        size_t *length);
int a20_flat_file_open(const char *path, int flags, vfile_t **out);
#endif

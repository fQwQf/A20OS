# littlefs source manifest (vendored v2.9.2, kernel/external/littlefs).
# Upstream sources are imported verbatim and are not restyled to the tree's
# conventions (same policy as lwIP); the VFS adapter lives in
# kernel/fs/diskfs/lfs_vfs.c.
LFILESDIR := kernel/external/littlefs
LFS_SRC = \
    $(LFILESDIR)/lfs.c \
    $(LFILESDIR)/lfs_util.c

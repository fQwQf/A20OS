/* Freestanding shim: littlefs heap calls route to the kernel allocator via
 * LFS_MALLOC/LFS_FREE (= lfs_kmalloc/lfs_kfree, defined in the adapter);
 * these declarations only satisfy the <stdlib.h> include. */
#ifndef LFS_COMPAT_STDLIB_H
#define LFS_COMPAT_STDLIB_H
void *lfs_kmalloc(unsigned long size);
void  lfs_kfree(void *ptr);
void *lfs_krealloc(void *ptr, unsigned long size);
#define malloc  lfs_kmalloc
#define free    lfs_kfree
#define realloc lfs_krealloc
#endif

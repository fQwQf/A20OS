/* Freestanding shim: littlefs assertions are compiled out in kernel builds
 * (LFS_NO_ASSERT is forced by the adapter's build flags); this header exists
 * so the unconditional include still resolves. */
#ifndef LFS_COMPAT_ASSERT_H
#define LFS_COMPAT_ASSERT_H
#endif

/* Freestanding shim: only the PRI macros littlefs uses. */
#ifndef LFS_COMPAT_INTTYPES_H
#define LFS_COMPAT_INTTYPES_H
#define PRId32 "d"
#define PRIu32 "u"
#define PRIx32 "x"
#define PRIX32 "X"
#define PRId64 "ld"
#define PRIu64 "lu"
#define PRIx64 "lx"
#endif

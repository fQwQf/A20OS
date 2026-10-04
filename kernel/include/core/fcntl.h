#ifndef _CORE_FCNTL_H
#define _CORE_FCNTL_H

/*
 * Kernel-internal fcntl.h constant namespace.
 * Values intentionally match the Linux ABI wire format; the ABI layer
 * re-exports them (abi/linux/...).  Internal code must include this
 * header, never anything under abi/.
 */
#define O_RDONLY     0
#define O_WRONLY     1
#define O_RDWR       2
#define O_ACCMODE    3
#define O_CREAT      0x40
#define O_TRUNC      0x200
#define O_APPEND     0x400
#define O_CLOEXEC    0x80000
#define O_NONBLOCK   0x800
#define O_EXCL       0x80
#define O_PATH       0x200000

/*
 * O_DIRECTORY/O_NOFOLLOW/O_DIRECT/O_LARGEFILE are NOT asm-generic: Linux
 * defines them per architecture in arch/<arch>/include/uapi/asm/fcntl.h, and
 * the layouts disagree.  A single hardcoded set silently misreads the wire
 * format of whichever arch it does not match.
 *
 * On PowerPC the generic O_DIRECTORY (00200000 = 0x10000) is in fact
 * O_LARGEFILE, which musl's open() passes on every call because the kernel
 * ABI always uses 64-bit off_t.  Reading that bit as O_DIRECTORY made
 * vfs_open() reject ENOTDIR on every open of a regular file or char device,
 * so `cat /hello.txt`, `cat /proc/version` and mksh's own /dev/tty all
 * failed while create-path opens (which take the !vn branch and never test
 * the bit) appeared to work.
 *
 * Keep the per-arch table in step with the kernel UAPI headers.
 */
#if defined(CONFIG_PPC64LE)
/* arch/powerpc: 040000 / 0100000 / 0400000 / 0200000 */
# define O_DIRECTORY  0x4000
# define O_NOFOLLOW   0x8000
# define O_DIRECT     0x20000
# define O_LARGEFILE  0x10000
#elif defined(CONFIG_ARM32) || defined(CONFIG_ARMV7M) || defined(CONFIG_AARCH64)
/* arch/arm and arch/arm64: 040000 / 0100000 / 0200000 / 0400000 */
# define O_DIRECTORY  0x4000
# define O_NOFOLLOW   0x8000
# define O_DIRECT     0x10000
# define O_LARGEFILE  0x20000
#else
/* asm-generic: 00200000 / 00400000 / 00040000 / 00100000 */
# define O_DIRECTORY  0x10000
# define O_NOFOLLOW   0x20000
# define O_DIRECT     0x4000
# define O_LARGEFILE  0x8000
#endif

#define STDIN_FILENO   0
#define STDOUT_FILENO  1
#define STDERR_FILENO  2

#define AT_FDCWD       (-100)
#define AT_SYMLINK_NOFOLLOW 0x100
#define AT_REMOVEDIR   0x200
#define AT_SYMLINK_FOLLOW 0x400
#define AT_NO_AUTOMOUNT 0x800
#define AT_EMPTY_PATH  0x1000
#define AT_EACCESS     0x200

#define RESOLVE_NO_XDEV            0x01
#define RESOLVE_NO_MAGICLINKS      0x02
#define RESOLVE_NO_SYMLINKS        0x04
#define RESOLVE_BENEATH            0x08
#define RESOLVE_IN_ROOT            0x10
#define RESOLVE_CACHED             0x20

#define RENAME_NOREPLACE           0x01
#define RENAME_EXCHANGE            0x02
#define RENAME_WHITEOUT            0x04

#define F_OK 0
#define X_OK 1
#define W_OK 2
#define R_OK 4

#define SEEK_SET  0
#define SEEK_CUR  1
#define SEEK_END  2

#define F_DUPFD   0
#define F_GETFD   1
#define F_SETFD   2
#define F_GETFL   3
#define F_SETFL   4
#define F_GETLK   5
#define F_SETLK   6
#define F_SETLKW  7
#define F_SETOWN  8
#define F_GETOWN  9
#define F_SETSIG  10
#define F_GETSIG  11
#define F_SETOWN_EX 15
#define F_GETOWN_EX 16
#define F_GETOWNER_UIDS 17
#define F_OFD_GETLK 36
#define F_OFD_SETLK 37
#define F_OFD_SETLKW 38
#define F_DUPFD_CLOEXEC 1030
#define F_SETLEASE 1024
#define F_GETLEASE 1025
#define F_NOTIFY 1026
#define F_CANCELLK 1029
#define F_SETPIPE_SZ 1031
#define F_GETPIPE_SZ 1032
#define F_ADD_SEALS 1033
#define F_GET_SEALS 1034
#define F_GET_RW_HINT 1035
#define F_SET_RW_HINT 1036
#define F_GET_FILE_RW_HINT 1037
#define F_SET_FILE_RW_HINT 1038
#define FD_CLOEXEC 1

#define LOCK_SH 1
#define LOCK_EX 2
#define LOCK_NB 4
#define LOCK_UN 8

#define F_RDLCK 0
#define F_WRLCK 1
#define F_UNLCK 2
#define F_OWNER_TID  0
#define F_OWNER_PID  1
#define F_OWNER_PGRP 2
#define F_SEAL_SEAL         0x0001
#define F_SEAL_SHRINK       0x0002
#define F_SEAL_GROW         0x0004
#define F_SEAL_WRITE        0x0008
#define F_SEAL_FUTURE_WRITE 0x0010

#define CAP_LEASE   28

#endif

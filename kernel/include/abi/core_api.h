#ifndef _ABI_CORE_API_H
#define _ABI_CORE_API_H

#include "core/types.h"
#include "fs/vfs.h"
#include "proc/proc.h"

/*
 * ABI_CORE_API_CONTRACT:
 * Linux and Native ABI code may translate user arguments and ABI error shapes,
 * but shared kernel semantics must be reached through stable proc/mm/vfs/net/ipc
 * entry points. This header is the narrow adapter surface for common calls that
 * both ABI personalities need today; expand it before adding ABI-private bypasses.
 */

static inline task_t *abi_core_proc_current(void)
{
    return proc_current();
}

static inline int64_t abi_core_proc_exec(const char *path, char **argv, char **envp)
{
    return proc_exec(path, argv, envp);
}

static inline uint64_t abi_core_proc_mmap(uint64_t addr, size_t len, int prot,
                                          int flags, int fd, uint64_t off)
{
    return proc_mmap(addr, len, prot, flags, fd, off);
}

static inline vfile_t *abi_core_vfs_get_file_ref(int fd)
{
    return vfs_get_file_ref(fd);
}

static inline void abi_core_vfs_put_file_ref(int fd, vfile_t *file)
{
    vfs_put_file_ref(fd, file);
}

static inline int64_t abi_core_vfs_read_file(vfile_t *file, void *buf, size_t count)
{
    return vfs_read_file(file, buf, count);
}

static inline int64_t abi_core_vfs_write_file(vfile_t *file, const void *buf, size_t count)
{
    return vfs_write_file(file, buf, count);
}

#endif

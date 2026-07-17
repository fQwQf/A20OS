/*
 * A20OS liba20c — fd ↔ handle mapping layer.
 * Maps POSIX fd (0-1023) to A20 handle indices.
 */
#include <stdint.h>
#include <stddef.h>
#include "fdtable.h"
#include "../liba20rt/a20_syscall.h"

#define FD_TABLE_INIT  32
#define FD_TABLE_MAX   1024

static struct __fd_entry *fd_table;
static int fd_table_cap;

extern void *__bare_alloc(size_t n);

int __fd_table_init(void)
{
    fd_table_cap = FD_TABLE_INIT;
    fd_table = (struct __fd_entry *)__bare_alloc(fd_table_cap * sizeof(struct __fd_entry));
    if (!fd_table) return -1;
    for (int i = 0; i < fd_table_cap; i++) {
        fd_table[i].handle     = 0xFFFFFFFF;
        fd_table[i].fd_flags   = 0;
        fd_table[i].open_flags = 0;
    }
    return 0;
}

int __fd_alloc(uint32_t handle)
{
    for (int i = 0; i < fd_table_cap; i++) {
        if (fd_table[i].handle == 0xFFFFFFFF) {
            fd_table[i].handle     = handle;
            fd_table[i].fd_flags   = 0;
            fd_table[i].open_flags = 0;
            return i;
        }
    }
    if (fd_table_cap < FD_TABLE_MAX) {
        int new_cap = fd_table_cap * 2;
        if (new_cap > FD_TABLE_MAX) new_cap = FD_TABLE_MAX;
        struct __fd_entry *new_tbl = (struct __fd_entry *)__bare_alloc(new_cap * sizeof(struct __fd_entry));
        if (!new_tbl) return -1;
        for (int i = 0; i < fd_table_cap; i++)
            new_tbl[i] = fd_table[i];
        for (int i = fd_table_cap; i < new_cap; i++) {
            new_tbl[i].handle     = 0xFFFFFFFF;
            new_tbl[i].fd_flags   = 0;
            new_tbl[i].open_flags = 0;
        }
        fd_table = new_tbl;
        fd_table_cap = new_cap;
        fd_table[fd_table_cap / 2].handle     = handle;
        fd_table[fd_table_cap / 2].fd_flags   = 0;
        fd_table[fd_table_cap / 2].open_flags = 0;
        return fd_table_cap / 2;
    }
    return -1;
}

uint32_t __fd_to_handle(int fd)
{
    if (fd < 0 || fd >= fd_table_cap) return 0xFFFFFFFF;
    return fd_table[fd].handle;
}

void __fd_free(int fd)
{
    if (fd >= 0 && fd < fd_table_cap) {
        fd_table[fd].handle     = 0xFFFFFFFF;
        fd_table[fd].fd_flags   = 0;
        fd_table[fd].open_flags = 0;
    }
}

int __fd_set(int fd, uint32_t handle)
{
    if (fd < 0 || fd >= FD_TABLE_MAX)
        return -1;

    if (fd >= fd_table_cap) {
        int new_cap = fd_table_cap * 2;
        while (new_cap <= fd)
            new_cap *= 2;
        if (new_cap > FD_TABLE_MAX)
            new_cap = FD_TABLE_MAX;
        if (new_cap <= fd_table_cap)
            return -1;

        struct __fd_entry *new_tbl = (struct __fd_entry *)__bare_alloc(new_cap * sizeof(struct __fd_entry));
        if (!new_tbl)
            return -1;
        for (int i = 0; i < fd_table_cap; i++)
            new_tbl[i] = fd_table[i];
        for (int i = fd_table_cap; i < new_cap; i++) {
            new_tbl[i].handle     = 0xFFFFFFFF;
            new_tbl[i].fd_flags   = 0;
            new_tbl[i].open_flags = 0;
        }
        fd_table     = new_tbl;
        fd_table_cap = new_cap;
    }

    fd_table[fd].handle     = handle;
    fd_table[fd].fd_flags   = 0;
    fd_table[fd].open_flags = 0;
    return 0;
}

int __fd_alloc_from(uint32_t handle, int min_fd)
{
    if (min_fd < 0) min_fd = 0;
    for (int i = min_fd; i < fd_table_cap; i++) {
        if (fd_table[i].handle == 0xFFFFFFFF) {
            fd_table[i].handle     = handle;
            fd_table[i].fd_flags   = 0;
            fd_table[i].open_flags = 0;
            return i;
        }
    }
    if (fd_table_cap < FD_TABLE_MAX) {
        int new_cap = fd_table_cap * 2;
        if (new_cap < min_fd + 1) new_cap = min_fd + 1;
        if (new_cap > FD_TABLE_MAX) new_cap = FD_TABLE_MAX;
        if (new_cap <= fd_table_cap) return -1;
        struct __fd_entry *new_tbl = (struct __fd_entry *)__bare_alloc(new_cap * sizeof(struct __fd_entry));
        if (!new_tbl) return -1;
        for (int i = 0; i < fd_table_cap; i++)
            new_tbl[i] = fd_table[i];
        for (int i = fd_table_cap; i < new_cap; i++) {
            new_tbl[i].handle     = 0xFFFFFFFF;
            new_tbl[i].fd_flags   = 0;
            new_tbl[i].open_flags = 0;
        }
        fd_table = new_tbl;
        fd_table_cap = new_cap;
        for (int i = min_fd; i < new_cap; i++) {
            if (fd_table[i].handle == 0xFFFFFFFF) {
                fd_table[i].handle     = handle;
                fd_table[i].fd_flags   = 0;
                fd_table[i].open_flags = 0;
                return i;
            }
        }
    }
    return -1;
}

void __fd_set_handle(int fd, uint32_t handle)
{
    if (fd >= 0 && fd < fd_table_cap)
        fd_table[fd].handle = handle;
}

void __fd_set_open_flags(int fd, uint32_t flags)
{
    if (fd >= 0 && fd < fd_table_cap)
        fd_table[fd].open_flags = flags;
}

uint32_t __fd_get_open_flags(int fd)
{
    if (fd < 0 || fd >= fd_table_cap) return 0;
    return fd_table[fd].open_flags;
}

void __fd_set_fd_flags(int fd, uint32_t flags)
{
    if (fd >= 0 && fd < fd_table_cap)
        fd_table[fd].fd_flags = flags;
}

uint32_t __fd_get_fd_flags(int fd)
{
    if (fd < 0 || fd >= fd_table_cap) return 0;
    return fd_table[fd].fd_flags;
}
